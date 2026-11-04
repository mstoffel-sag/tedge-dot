//! Push delivery (spec §5.2–§5.4): one task per subscribed device that delivers its pushed
//! points through the Channels WebSocket or by recorded polling, catches up from the delivery
//! cursors after an outage or a restart, and reports its liveness to `check_subscription`.
//!
//! `start` returns as soon as the channel is open (or the recorded loop is set up): the
//! catch-up runs in the task, never inside `subscribe`, which the runtime bounds with
//! `operation_timeout`. Every sample goes through the runtime's bounded sink, so a catch-up is
//! paced by the runtime and never outruns it.

use crate::client::{chunks, query, Channel, ChunkSize, Client, HttpError};
use crate::config::{Point, Settings};
use crate::cursor::{Stamp, Store};
use crate::resolve::Stream;
use crate::sample;
use crate::value::errors_of;
use futures_util::StreamExt;
use serde_json::Value as Json;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};
use tedge_dot_sdk::SampleSink;
use time::OffsetDateTime;
use tokio::sync::mpsc;
use tokio::task::JoinHandle;
use tokio_tungstenite::tungstenite::Message;
use tracing::{debug, info, warn};

/// The most events a `recorded` request may ask for (the server's default
/// `MaxReturnedItemsPerCall`).
const MAX_COUNT: usize = 150_000;

/// A pushed point and the stream it was resolved to.
#[derive(Clone, Debug)]
pub struct Target {
    pub point: Point,
    pub stream: Stream,
}

/// What `check_subscription` reads.
#[derive(Debug)]
pub struct Health {
    pub last_alive: Instant,
    pub failed: Option<String>,
}

impl Health {
    fn alive(health: &Mutex<Health>) {
        health.lock().unwrap().last_alive = Instant::now();
    }
}

/// Why push delivery could not start.
#[derive(Debug)]
pub enum StartError {
    /// The WebSocket upgrade was answered with this status (not 101): fall back to recorded.
    Refused(u16, String),
    Http(HttpError),
}

/// Aborts its tasks when dropped.
struct Aborting(Vec<JoinHandle<()>>);

impl Drop for Aborting {
    fn drop(&mut self) {
        for t in &self.0 {
            t.abort();
        }
    }
}

/// A running device task.
pub struct Push {
    task: JoinHandle<()>,
    pub health: Arc<Mutex<Health>>,
    /// The longest silence that is still normal, divided by three.
    pub period: Option<Duration>,
    pub store: Arc<Mutex<Store>>,
}

impl Drop for Push {
    fn drop(&mut self) {
        self.task.abort();
    }
}

impl Push {
    /// `Ok` while delivery is live (spec §5.2 liveness).
    pub fn check(&self) -> Result<(), String> {
        let h = self.health.lock().unwrap();
        if let Some(reason) = &h.failed {
            return Err(reason.clone());
        }
        if let Some(p) = self.period {
            let silent = h.last_alive.elapsed();
            if silent > p * 3 {
                return Err(format!("no message or heartbeat for {:.1}s", silent.as_secs_f64()));
            }
        }
        Ok(())
    }

    /// Stop the task and write the cursors.
    pub fn stop(self) {
        self.task.abort();
        self.store.lock().unwrap().flush();
    }
}

struct Ctx {
    device: String,
    client: Arc<Client>,
    chunk: Arc<ChunkSize>,
    settings: Settings,
    targets: Vec<Target>,
    store: Arc<Mutex<Store>>,
    health: Arc<Mutex<Health>>,
    sink: SampleSink,
    interval: Duration,
}

impl Ctx {
    fn fail(&self, reason: String) {
        warn!(device = %self.device, "push delivery stopped: {reason}");
        self.health.lock().unwrap().failed = Some(reason);
    }

    fn web_ids(&self) -> Vec<String> {
        let mut ids: Vec<String> = Vec::new();
        for t in &self.targets {
            if !ids.contains(&t.stream.web_id) {
                ids.push(t.stream.web_id.clone());
            }
        }
        ids
    }

    /// Hand one PI event of `target` to the runtime, unless the cursor has it already. Returns
    /// whether it was delivered; `Err` when the runtime is gone.
    async fn deliver(&self, target: &Target, ev: &Json, replayed: bool) -> Result<bool, String> {
        let Some(ts) = ev.get("Timestamp").and_then(Json::as_str).and_then(Stamp::parse) else {
            debug!(device = %self.device, point = %target.point.id, "an event without a valid Timestamp is skipped");
            return Ok(false);
        };
        let value = ev.get("Value").cloned().unwrap_or(Json::Null);
        if let Some(c) = self.store.lock().unwrap().get(&target.point.id) {
            if !c.admits(ts.at, &value) {
                if ts.at < c.ts.at {
                    debug!(device = %self.device, point = %target.point.id, ts = %ts.text,
                           "an event older than the delivery cursor (a history edit) is dropped");
                }
                return Ok(false);
            }
        }
        let s = sample::build(&self.device, &target.point, &target.stream, ev, Some(ts.at), replayed);
        self.sink.send(s).await.map_err(|_| "the runtime stopped".to_string())?;
        let mut store = self.store.lock().unwrap();
        store.advance(&target.point.id, &target.stream.web_id, &ts, &value);
        store.maybe_flush();
        Ok(true)
    }

    /// Deliver the events of one stream item (a channel message's or a recorded answer's), in
    /// time order: a channel does not guarantee it.
    async fn deliver_item(&self, item: &Json, replayed: bool) -> Result<usize, String> {
        let Some(web_id) = item.get("WebId").and_then(Json::as_str) else { return Ok(0) };
        if let Some(e) = errors_of(item) {
            warn!(device = %self.device, %web_id, "PI reported an error for the stream: {e}");
            return Ok(0);
        }
        let mut events: Vec<(OffsetDateTime, &Json)> = item
            .get("Items")
            .and_then(Json::as_array)
            .map(|evs| {
                evs.iter()
                    .filter_map(|e| Some((Stamp::parse(e.get("Timestamp")?.as_str()?)?.at, e)))
                    .collect()
            })
            .unwrap_or_default();
        events.sort_by_key(|(at, _)| *at);
        let mut n = 0;
        for target in self.targets.iter().filter(|t| t.stream.web_id == web_id) {
            for (_, ev) in &events {
                if self.deliver(target, ev, replayed).await? {
                    n += 1;
                }
            }
        }
        Ok(n)
    }

    /// Read `target`'s archive from `from` in pages (spec §5.3, §5.4) and deliver it.
    async fn page(&self, target: &Target, from: Stamp, max_events: Option<usize>, replayed: bool) -> Result<(), String> {
        let page = self.settings.backfill.as_ref().map_or(1000, |b| b.page_size);
        let mut max_count = page;
        let mut from = from;
        let mut delivered = 0usize;
        loop {
            let q = query(&[
                ("webId", &target.stream.web_id),
                ("startTime", &from.text),
                ("endTime", "*"),
                ("maxCount", &max_count.to_string()),
                ("boundaryType", "Inside"),
                ("selectedFields", "Items.WebId;Items.Items;Items.Errors"),
            ]);
            let doc = self.client.get("streamsets/recorded", &q).await.map_err(|e| e.to_string())?;
            Health::alive(&self.health);
            let Some(item) = doc.get("Items").and_then(Json::as_array).and_then(|a| a.first()) else {
                return Ok(());
            };
            if let Some(e) = errors_of(item) {
                warn!(device = %self.device, point = %target.point.id, "cannot read the archive: {e}");
                return Ok(());
            }
            let events: Vec<&Json> = item.get("Items").and_then(Json::as_array).map(|a| a.iter().collect()).unwrap_or_default();
            let n = events.len();
            let mut stamped: Vec<(Stamp, &Json)> = events
                .into_iter()
                .filter_map(|e| Some((Stamp::parse(e.get("Timestamp")?.as_str()?)?, e)))
                .collect();
            stamped.sort_by_key(|(s, _)| s.at);
            for (ts, ev) in &stamped {
                if self.deliver(target, ev, replayed).await? {
                    delivered += 1;
                    if max_events.is_some_and(|max| delivered >= max) {
                        warn!(device = %self.device, point = %target.point.id,
                              "backfill.max_events ({delivered}) reached: events after {} are not replayed", ts.text);
                        return Ok(());
                    }
                }
            }
            if n < max_count {
                return Ok(());
            }
            let (Some(first), Some(last)) = (stamped.first(), stamped.last()) else { return Ok(()) };
            if first.0.at == last.0.at {
                // A whole page at one timestamp: ask for more at once, or paging would stall.
                if max_count >= MAX_COUNT {
                    warn!(device = %self.device, point = %target.point.id,
                          "more than {MAX_COUNT} events at {}: the rest are skipped", last.0.text);
                    return Ok(());
                }
                max_count = (max_count * 2).min(MAX_COUNT);
            } else {
                from = last.0.clone();
                max_count = page;
            }
        }
    }

    /// Catch up every point that has a cursor (spec §5.4): from the cursor, or from
    /// `now - backfill.max_age` for a longer gap.
    async fn catch_up(&self) -> Result<(), String> {
        let Some(backfill) = self.settings.backfill.clone() else { return Ok(()) };
        let floor = OffsetDateTime::now_utc() - backfill.max_age;
        for target in &self.targets {
            let Some(cursor) = self.store.lock().unwrap().get(&target.point.id).cloned() else { continue };
            let from = if cursor.ts.at < floor {
                let floor = Stamp::of(floor);
                warn!(device = %self.device, point = %target.point.id,
                      "events from {} to {} are older than backfill.max_age and are not replayed",
                      cursor.ts.text, floor.text);
                floor
            } else {
                cursor.ts
            };
            info!(device = %self.device, point = %target.point.id, from = %from.text, "catching up from the archive");
            self.page(target, from, Some(backfill.max_events), true).await?;
        }
        Ok(())
    }

    /// Open the device's channels, one per chunk of WebIds.
    async fn open_channels(&self, include_initial: bool) -> Result<Vec<Channel>, StartError> {
        let ids = self.web_ids();
        let heartbeat = self.settings.channel.heartbeat_rate.to_string();
        let initial = include_initial.to_string();
        let mut out = Vec::new();
        let mut pending: Vec<Vec<String>> = chunks(&ids, self.chunk.get(), self.settings.max_url_bytes, 200);
        while let Some(chunk) = pending.pop() {
            let mut pairs: Vec<(&str, &str)> = chunk.iter().map(|w| ("webId", w.as_str())).collect();
            pairs.push(("includeInitialValues", &initial));
            pairs.push(("heartbeatRate", &heartbeat));
            match self.client.channel(&query(&pairs)).await {
                Ok(ws) => out.push(ws),
                Err(HttpError::TooLong(code)) if self.chunk.halve(&self.device, chunk.len(), code) => {
                    let half = chunk.len().div_ceil(2);
                    pending.push(chunk[half..].to_vec());
                    pending.push(chunk[..half].to_vec());
                }
                Err(HttpError::Status { code, errors }) => return Err(StartError::Refused(code, errors)),
                Err(e) => return Err(StartError::Http(e)),
            }
        }
        Ok(out)
    }
}

enum Incoming {
    Message(Json),
    Closed(String),
}

/// Read one channel: every frame proves the server alive (a heartbeat too); data and error
/// messages go to the device task through the bounded buffer. When the buffer is full the
/// reader stops (closing its socket) and raises `overflow`.
fn reader(mut ws: Channel, tx: mpsc::Sender<Incoming>, health: Arc<Mutex<Health>>, overflow: Arc<AtomicBool>) -> JoinHandle<()> {
    tokio::spawn(async move {
        loop {
            let reason = match ws.next().await {
                Some(Ok(Message::Text(text))) => {
                    Health::alive(&health);
                    let Ok(json) = serde_json::from_str::<Json>(&text) else { continue };
                    let empty = json.get("Items").and_then(Json::as_array).is_some_and(Vec::is_empty);
                    if empty && json.get("Errors").is_none() {
                        continue; // a heartbeat
                    }
                    match tx.try_send(Incoming::Message(json)) {
                        Ok(()) => continue,
                        Err(mpsc::error::TrySendError::Full(_)) => {
                            overflow.store(true, Ordering::SeqCst);
                            return;
                        }
                        Err(mpsc::error::TrySendError::Closed(_)) => return,
                    }
                }
                Some(Ok(Message::Close(_))) | None => "the server closed the channel".to_string(),
                Some(Ok(_)) => {
                    Health::alive(&health);
                    continue;
                }
                Some(Err(e)) => format!("channel: {e}"),
            };
            let _ = tx.send(Incoming::Closed(reason)).await;
            return;
        }
    })
}

async fn channel_task(ctx: Ctx, mut sockets: Vec<Channel>) {
    loop {
        let buffer = ctx.settings.backfill.as_ref().map_or(10_000, |b| b.live_buffer).max(1);
        let (tx, mut rx) = mpsc::channel(buffer);
        let overflow = Arc::new(AtomicBool::new(false));
        let readers = Aborting(
            sockets
                .drain(..)
                .map(|ws| reader(ws, tx.clone(), ctx.health.clone(), overflow.clone()))
                .collect(),
        );
        drop(tx);
        if let Err(e) = ctx.catch_up().await {
            return ctx.fail(e);
        }
        let mut tick = tokio::time::interval(Duration::from_secs(1));
        loop {
            if overflow.load(Ordering::SeqCst) {
                break;
            }
            tokio::select! {
                m = rx.recv() => match m {
                    Some(Incoming::Message(json)) => {
                        if let Some(e) = errors_of(&json) {
                            warn!(device = %ctx.device, "PI Web API reported on the channel: {e}");
                            continue;
                        }
                        for item in json.get("Items").and_then(Json::as_array).into_iter().flatten() {
                            if let Err(e) = ctx.deliver_item(item, false).await {
                                return ctx.fail(e);
                            }
                        }
                    }
                    Some(Incoming::Closed(reason)) => return ctx.fail(reason),
                    None if overflow.load(Ordering::SeqCst) => break,
                    None => return ctx.fail("the channel ended".into()),
                },
                _ = tick.tick() => ctx.store.lock().unwrap().maybe_flush(),
            }
        }
        // More live events arrived than the buffer holds (spec §5.4, live buffer): deliver what
        // is buffered, catch up from the cursor with recorded reads, and open the channel again.
        warn!(device = %ctx.device, "the live buffer is full; catching up from the archive before reopening the channel");
        drop(readers);
        while let Ok(m) = rx.try_recv() {
            if let Incoming::Message(json) = m {
                for item in json.get("Items").and_then(Json::as_array).into_iter().flatten() {
                    if let Err(e) = ctx.deliver_item(item, false).await {
                        return ctx.fail(e);
                    }
                }
            }
        }
        if let Err(e) = ctx.catch_up().await {
            return ctx.fail(e);
        }
        sockets = match ctx.open_channels(false).await {
            Ok(s) => s,
            Err(StartError::Refused(code, _)) => return ctx.fail(format!("reopening the channel: HTTP {code}")),
            Err(StartError::Http(e)) => return ctx.fail(format!("reopening the channel: {e}")),
        };
    }
}

async fn recorded_task(ctx: Ctx) {
    if let Err(e) = recorded(&ctx).await {
        ctx.fail(e);
    }
}

async fn recorded(ctx: &Ctx) -> Result<(), String> {
    ctx.catch_up().await?;
    // A point without a cursor starts at its current value (as a channel's initial values do).
    let fresh: Vec<&Target> = ctx
        .targets
        .iter()
        .filter(|t| ctx.store.lock().unwrap().get(&t.point.id).is_none())
        .collect();
    if !fresh.is_empty() {
        let ids: Vec<String> = fresh.iter().map(|t| t.stream.web_id.clone()).collect();
        for chunk in chunks(&ids, ctx.chunk.get(), ctx.settings.max_url_bytes, 200) {
            let mut pairs: Vec<(&str, &str)> = chunk.iter().map(|w| ("webId", w.as_str())).collect();
            pairs.push(("selectedFields", "Items.WebId;Items.Value;Items.Errors"));
            let doc = ctx.client.get("streamsets/value", &query(&pairs)).await.map_err(|e| e.to_string())?;
            Health::alive(&ctx.health);
            for item in doc.get("Items").and_then(Json::as_array).into_iter().flatten() {
                let (Some(web_id), Some(value)) = (item.get("WebId").and_then(Json::as_str), item.get("Value")) else { continue };
                for t in fresh.iter().filter(|t| t.stream.web_id == web_id) {
                    ctx.deliver(t, value, false).await?;
                }
            }
        }
    }
    let mut next = tokio::time::Instant::now() + ctx.interval;
    loop {
        tokio::time::sleep_until(next).await;
        next += ctx.interval;
        poll(ctx).await?;
        Health::alive(&ctx.health);
        ctx.store.lock().unwrap().maybe_flush();
    }
}

/// One recorded poll of every pushed point (spec §5.3): from the oldest cursor of each chunk,
/// each point keeping only what is after its own cursor. Events read after the fact are
/// replayed samples.
async fn poll(ctx: &Ctx) -> Result<(), String> {
    let page = ctx.settings.backfill.as_ref().map_or(1000, |b| b.page_size);
    let ids = ctx.web_ids();
    for chunk in chunks(&ids, ctx.chunk.get(), ctx.settings.max_url_bytes, 300) {
        let start = {
            let store = ctx.store.lock().unwrap();
            ctx.targets
                .iter()
                .filter(|t| chunk.contains(&t.stream.web_id))
                .filter_map(|t| store.get(&t.point.id).map(|c| c.ts.clone()))
                .min_by_key(|s| s.at)
        };
        let start = start.unwrap_or_else(|| Stamp::of(OffsetDateTime::now_utc() - ctx.interval));
        let max_count = page.to_string();
        let mut pairs: Vec<(&str, &str)> = chunk.iter().map(|w| ("webId", w.as_str())).collect();
        pairs.extend([
            ("startTime", start.text.as_str()),
            ("endTime", "*"),
            ("maxCount", max_count.as_str()),
            ("boundaryType", "Inside"),
            ("selectedFields", "Items.WebId;Items.Items;Items.Errors"),
        ]);
        let doc = match ctx.client.get("streamsets/recorded", &query(&pairs)).await {
            Ok(doc) => doc,
            Err(HttpError::TooLong(code)) if ctx.chunk.halve(&ctx.device, chunk.len(), code) => return Ok(()),
            Err(e) => return Err(e.to_string()),
        };
        for item in doc.get("Items").and_then(Json::as_array).into_iter().flatten() {
            ctx.deliver_item(item, true).await?;
            let full = item.get("Items").and_then(Json::as_array).is_some_and(|a| a.len() >= page);
            if full {
                // More than one page since the cursor: page on from where this one ended.
                let web_id = item.get("WebId").and_then(Json::as_str).unwrap_or_default();
                for t in ctx.targets.iter().filter(|t| t.stream.web_id == web_id) {
                    let from = ctx.store.lock().unwrap().get(&t.point.id).map(|c| c.ts.clone());
                    if let Some(from) = from {
                        ctx.page(t, from, None, true).await?;
                    }
                }
            }
        }
    }
    Ok(())
}

/// Start push delivery for a device. `recorded`: poll the archive instead of opening channels.
#[allow(clippy::too_many_arguments)]
pub async fn start(
    device: &str,
    client: Arc<Client>,
    chunk: Arc<ChunkSize>,
    settings: &Settings,
    recorded: bool,
    targets: Vec<Target>,
    store: Arc<Mutex<Store>>,
    interval: Duration,
    sink: SampleSink,
) -> Result<Push, StartError> {
    let health = Arc::new(Mutex::new(Health { last_alive: Instant::now(), failed: None }));
    let ctx = Ctx {
        device: device.to_string(),
        client,
        chunk,
        settings: settings.clone(),
        targets,
        store: store.clone(),
        health: health.clone(),
        sink,
        interval,
    };
    let (task, period) = if recorded {
        (tokio::spawn(recorded_task(ctx)), Some(interval))
    } else {
        let include = settings.channel.include_initial_values;
        let sockets = ctx.open_channels(include).await?;
        let period = settings.channel.heartbeat_period();
        (tokio::spawn(channel_task(ctx, sockets)), period)
    };
    Ok(Push { task, health, period, store })
}
