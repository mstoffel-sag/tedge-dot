//! Concurrent reads (contract §5.6): how many requests may be in flight to one endpoint, and in
//! all.
//!
//! Every device has an **endpoint**, named by its module ([`crate::Connector::endpoint`]): the
//! host and port it talks to, a serial port, a CAN interface. Devices that share one share its
//! budget, `max_inflight` (default 4; the smallest of their values when they differ), and the
//! module's cap for the endpoint always applies (1 for a bus). `max_inflight_total` (default 32)
//! bounds the requests in flight across all endpoints.
//!
//! A read takes one request of its endpoint's budget, waiting if none is free, then as many more
//! as are free, up to what the module asks for ([`Budgets::acquire`]); every device on an
//! endpoint therefore makes progress. The module sends at most that many requests at once
//! ([`InFlight::allowance`]) and reports refusals and timeouts ([`InFlight::busy`],
//! [`InFlight::timed_out`]): an endpoint that says it is busy has its budget halved, and regains
//! one step after [`REGROW_AFTER`] successful requests ([`InFlight::ok`]).
//!
//! The C SDK (impl/c/sdk/src/concurrency.c) implements the same rules.

use crate::config::ConnectorConfig;
use std::collections::HashMap;
use std::sync::{Arc, Mutex};
use tokio::sync::Notify;
use tracing::info;

/// Requests in flight to one endpoint unless configured otherwise.
pub const DEFAULT_MAX_INFLIGHT: usize = 4;
/// Requests in flight across all endpoints unless configured otherwise.
pub const DEFAULT_MAX_INFLIGHT_TOTAL: usize = 32;
/// Successful requests at a reduced budget before it grows by one.
pub const REGROW_AFTER: u32 = 20;

/// `key` of a `[connector]` or device table, when present, must be a positive integer.
pub fn check_positive(table: &toml::Value, key: &str) -> Result<(), String> {
    match table.get(key) {
        None => Ok(()),
        Some(v) if v.as_integer().is_some_and(|n| n > 0) => Ok(()),
        Some(_) => Err(format!("{key} must be a positive integer")),
    }
}

fn configured(value: Option<i64>) -> Option<usize> {
    value.and_then(|n| usize::try_from(n).ok()).filter(|n| *n > 0)
}

#[derive(Debug)]
struct State {
    /// The effective budget: the configured one, or less after a backoff.
    limit: usize,
    in_use: usize,
    /// Successful requests since the budget last changed.
    successes: u32,
}

/// One endpoint's budget, or the total one.
#[derive(Debug)]
pub struct Budget {
    name: String,
    configured: usize,
    /// Whether refusals lower it (an endpoint) or not (the total).
    backs_off: bool,
    state: Mutex<State>,
    freed: Notify,
}

impl Budget {
    fn new(name: String, configured: usize, backs_off: bool) -> Self {
        Budget {
            name,
            configured,
            backs_off,
            state: Mutex::new(State { limit: configured, in_use: 0, successes: 0 }),
            freed: Notify::new(),
        }
    }

    pub fn name(&self) -> &str {
        &self.name
    }

    pub fn configured(&self) -> usize {
        self.configured
    }

    /// The budget now, after any backoff.
    pub fn effective(&self) -> usize {
        self.state.lock().unwrap().limit
    }

    fn try_take(&self) -> bool {
        let mut s = self.state.lock().unwrap();
        if s.in_use < s.limit {
            s.in_use += 1;
            true
        } else {
            false
        }
    }

    async fn take(&self) {
        loop {
            let freed = self.freed.notified();
            if self.try_take() {
                return;
            }
            freed.await;
        }
    }

    fn give_back(&self, n: usize) {
        if n == 0 {
            return;
        }
        let mut s = self.state.lock().unwrap();
        s.in_use = s.in_use.saturating_sub(n);
        drop(s);
        self.freed.notify_waiters();
    }

    /// The endpoint refused a request as busy, or one timed out while others were in flight.
    fn back_off(&self, why: &str) {
        if !self.backs_off {
            return;
        }
        let mut s = self.state.lock().unwrap();
        let halved = (s.limit / 2).max(1);
        s.successes = 0;
        if halved != s.limit {
            s.limit = halved;
            info!(endpoint = %self.name, budget = halved, "{why}: requests in flight lowered");
        }
    }

    fn success(&self) {
        let mut s = self.state.lock().unwrap();
        if s.limit >= self.configured {
            return;
        }
        s.successes += 1;
        if s.successes >= REGROW_AFTER {
            s.successes = 0;
            s.limit += 1;
            info!(endpoint = %self.name, budget = s.limit, "requests in flight raised");
            drop(s);
            self.freed.notify_waiters();
        }
    }
}

/// The budgets of a configuration: one per endpoint, and the total.
#[derive(Debug, Clone)]
pub struct Budgets {
    total: Arc<Budget>,
    /// By device index.
    devices: Vec<Arc<Budget>>,
    /// By endpoint name, with its devices' names, for the capability descriptor.
    endpoints: Vec<(Arc<Budget>, Vec<String>)>,
}

impl Budgets {
    /// `endpoint_of` names each device's endpoint and the module's cap for it (`None`: the
    /// device is its own endpoint, uncapped).
    pub fn new(
        config: &ConnectorConfig,
        endpoint_of: impl Fn(&str) -> Option<(String, Option<usize>)>,
    ) -> Self {
        let default = configured(config.connector.max_inflight).unwrap_or(DEFAULT_MAX_INFLIGHT);
        let total = configured(config.connector.max_inflight_total).unwrap_or(DEFAULT_MAX_INFLIGHT_TOTAL);
        // name -> (budget, cap, devices), in the order endpoints first appear
        let mut order: Vec<String> = Vec::new();
        let mut table: HashMap<String, (usize, Option<usize>, Vec<String>)> = HashMap::new();
        let mut names = Vec::with_capacity(config.devices.len());
        for device in &config.devices {
            let (name, cap) = endpoint_of(&device.name).unwrap_or_else(|| (device.name.clone(), None));
            let wanted = configured(device.max_inflight).unwrap_or(default);
            let entry = table.entry(name.clone()).or_insert_with(|| {
                order.push(name.clone());
                (wanted, cap, Vec::new())
            });
            entry.0 = entry.0.min(wanted);
            entry.1 = match (entry.1, cap) {
                (Some(a), Some(b)) => Some(a.min(b)),
                (a, b) => a.or(b),
            };
            entry.2.push(device.name.clone());
            names.push(name);
        }
        let budgets: HashMap<String, Arc<Budget>> = order
            .iter()
            .map(|name| {
                let (wanted, cap, _) = &table[name];
                let limit = cap.map_or(*wanted, |c| (*wanted).min(c.max(1)));
                (name.clone(), Arc::new(Budget::new(name.clone(), limit, true)))
            })
            .collect();
        Budgets {
            total: Arc::new(Budget::new("total".into(), total, false)),
            devices: names.iter().map(|n| budgets[n].clone()).collect(),
            endpoints: order
                .iter()
                .map(|n| (budgets[n].clone(), table[n].2.clone()))
                .collect(),
        }
    }

    /// The endpoint budget of device `index`.
    pub fn endpoint(&self, index: usize) -> &Arc<Budget> {
        &self.devices[index]
    }

    /// Take the requests a read of device `index` may send at once: one, waiting until its
    /// endpoint and the total each have one free, then up to `wanted - 1` more that are free now.
    pub async fn acquire(&self, index: usize, wanted: usize) -> InFlight {
        let endpoint = self.devices[index].clone();
        endpoint.take().await;
        self.total.take().await;
        let mut held = 1;
        while held < wanted.max(1) {
            if !endpoint.try_take() {
                break;
            }
            if !self.total.try_take() {
                endpoint.give_back(1);
                break;
            }
            held += 1;
        }
        InFlight { endpoint, total: self.total.clone(), held }
    }

    /// The capability descriptor's `endpoints`: each endpoint with its devices and its budget.
    /// A backoff changes the budget in effect, which is logged, not republished.
    pub fn describe(&self) -> serde_json::Value {
        serde_json::Value::Array(
            self.endpoints
                .iter()
                .map(|(b, devices)| {
                    serde_json::json!({
                        "endpoint": b.name(),
                        "devices": devices,
                        "max_inflight": b.configured(),
                    })
                })
                .collect(),
        )
    }
}

/// The requests a read holds while it runs. Dropping it gives them back.
#[derive(Debug)]
pub struct InFlight {
    endpoint: Arc<Budget>,
    total: Arc<Budget>,
    held: usize,
}

impl InFlight {
    /// How many requests the read may have in flight at once. It never grows during a call; it
    /// shrinks when the endpoint backs off ([`InFlight::current`]).
    pub fn allowance(&self) -> usize {
        self.held
    }

    /// What the read may have in flight now: its allowance, or less once the endpoint backed
    /// off. Never below 1.
    pub fn current(&self) -> usize {
        self.held.min(self.endpoint.effective()).max(1)
    }

    /// A request was answered.
    pub fn ok(&self) {
        self.endpoint.success();
    }

    /// A request was refused because the endpoint is busy (OPC UA `BadTooManyOperations`,
    /// `BadServerTooBusy`, `BadTooManySessions`).
    pub fn busy(&self) {
        self.endpoint.back_off("endpoint busy");
    }

    /// A request timed out. It backs off only when other requests to the endpoint were in
    /// flight: alone, it is an ordinary link failure.
    pub fn timed_out(&self, others_in_flight: bool) {
        if others_in_flight {
            self.endpoint.back_off("request timed out behind others");
        }
    }

    /// A read that is not run under the budgets (the CLI, tests): one request at a time.
    pub fn single() -> Self {
        let b = Arc::new(Budget::new("single".into(), 1, false));
        b.try_take();
        InFlight { endpoint: b.clone(), total: b, held: 1 }
    }
}

impl Drop for InFlight {
    fn drop(&mut self) {
        if Arc::ptr_eq(&self.endpoint, &self.total) {
            self.endpoint.give_back(self.held);
            return;
        }
        self.endpoint.give_back(self.held);
        self.total.give_back(self.held);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::Duration;

    fn config(text: &str) -> ConnectorConfig {
        crate::library::resolve(text, std::path::Path::new(".")).unwrap()
    }

    const TWO_ON_ONE: &str = "[connector]\nprotocol = \"bench\"\n\
        [[device]]\nname = \"a\"\nprotocol_address = {}\n\
        [[device]]\nname = \"b\"\nprotocol_address = {}\nmax_inflight = 2\n\
        [[device]]\nname = \"c\"\nprotocol_address = {}\n";

    fn shared(name: &str) -> Option<(String, Option<usize>)> {
        Some((if name == "c" { "other" } else { "plc" }.to_string(), None))
    }

    #[test]
    fn the_smallest_budget_of_an_endpoint_wins() {
        let b = Budgets::new(&config(TWO_ON_ONE), shared);
        assert_eq!(b.endpoint(0).configured(), 2);
        assert!(Arc::ptr_eq(b.endpoint(0), b.endpoint(1)));
        assert_eq!(b.endpoint(2).configured(), DEFAULT_MAX_INFLIGHT);
        assert_eq!(
            b.describe()[0],
            serde_json::json!({ "endpoint": "plc", "devices": ["a", "b"], "max_inflight": 2 })
        );
    }

    #[test]
    fn the_module_cap_wins() {
        let b = Budgets::new(&config(TWO_ON_ONE), |_| Some(("/dev/ttyUSB0".into(), Some(1))));
        assert_eq!(b.endpoint(0).configured(), 1);
        let none = Budgets::new(&config(TWO_ON_ONE), |_| None);
        assert_eq!(none.endpoint(0).name(), "a");
        assert_eq!(none.endpoint(1).configured(), 2);
    }

    #[tokio::test]
    async fn a_read_takes_one_then_what_is_free() {
        let b = Budgets::new(&config(TWO_ON_ONE), |_| Some(("plc".into(), None)));
        let first = b.acquire(0, 4).await;
        assert_eq!(first.allowance(), 2);
        // The endpoint is full: a second device waits for one request, and gets it.
        let waiter = {
            let b = b.clone();
            tokio::spawn(async move { b.acquire(2, 4).await.allowance() })
        };
        tokio::time::sleep(Duration::from_millis(20)).await;
        assert!(!waiter.is_finished());
        drop(first);
        assert_eq!(waiter.await.unwrap(), 2);
    }

    #[tokio::test]
    async fn every_device_gets_a_request() {
        let b = Budgets::new(&config(TWO_ON_ONE), |_| Some(("plc".into(), None)));
        // Device a takes both requests of the endpoint (budget 2); b and c each still get one
        // in turn as a's read ends, rather than a taking them all again.
        let a = b.acquire(0, 2).await;
        let others: Vec<_> = (1..3)
            .map(|i| {
                let b = b.clone();
                tokio::spawn(async move { b.acquire(i, 1).await.allowance() })
            })
            .collect();
        drop(a);
        for o in others {
            assert_eq!(o.await.unwrap(), 1);
        }
    }

    #[tokio::test]
    async fn the_total_bounds_every_endpoint() {
        let text = "[connector]\nprotocol = \"bench\"\nmax_inflight_total = 3\n\
            [[device]]\nname = \"a\"\nprotocol_address = {}\n[[device]]\nname = \"b\"\nprotocol_address = {}\n";
        let b = Budgets::new(&config(text), |_| None);
        let a = b.acquire(0, 4).await;
        assert_eq!(a.allowance(), 3);
        let waiter = {
            let b = b.clone();
            tokio::spawn(async move { b.acquire(1, 4).await.allowance() })
        };
        tokio::time::sleep(Duration::from_millis(20)).await;
        assert!(!waiter.is_finished());
        drop(a);
        assert_eq!(waiter.await.unwrap(), 3);
    }

    #[tokio::test]
    async fn busy_halves_and_successes_regrow() {
        let text = "[connector]\nprotocol = \"bench\"\nmax_inflight = 8\n[[device]]\nname = \"a\"\nprotocol_address = {}\n";
        let b = Budgets::new(&config(text), |_| None);
        let read = b.acquire(0, 8).await;
        assert_eq!(read.allowance(), 8);
        read.busy();
        assert_eq!(b.endpoint(0).effective(), 4);
        assert_eq!(read.current(), 4);
        read.busy();
        read.busy();
        read.busy();
        assert_eq!(b.endpoint(0).effective(), 1, "never below one");
        for _ in 0..REGROW_AFTER {
            read.ok();
        }
        assert_eq!(b.endpoint(0).effective(), 2);
        // A timeout alone is a link failure, not a busy endpoint.
        read.timed_out(false);
        assert_eq!(b.endpoint(0).effective(), 2);
        read.timed_out(true);
        assert_eq!(b.endpoint(0).effective(), 1);
    }

    #[test]
    fn values_are_positive_integers() {
        let t: toml::Value = toml::from_str("max_inflight = 0\nmax_inflight_total = \"many\"\nok = 3").unwrap();
        assert_eq!(check_positive(&t, "max_inflight").unwrap_err(), "max_inflight must be a positive integer");
        assert_eq!(check_positive(&t, "max_inflight_total").unwrap_err(), "max_inflight_total must be a positive integer");
        assert!(check_positive(&t, "ok").is_ok());
        assert!(check_positive(&t, "absent").is_ok());
    }
}
