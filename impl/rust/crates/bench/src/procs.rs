//! Processes the harness starts and measures: CPU time and memory per PID, names (so a PID
//! that was reused is never killed), and termination. Processes are only ever addressed by the
//! PID the harness started, never by a name pattern.

use std::io;
use std::path::Path;
use std::process::{Child, Command, Stdio};
use std::time::{Duration, Instant};

/// Cumulative user + system CPU time of `pid`, in seconds.
#[cfg(target_os = "linux")]
pub fn cpu_seconds(pid: u32) -> io::Result<f64> {
    let stat = std::fs::read_to_string(format!("/proc/{pid}/stat"))?;
    // The command name (field 2) may contain spaces; the fields after it are fixed.
    let rest = stat.rsplit_once(')').map(|(_, r)| r).unwrap_or("");
    let f: Vec<&str> = rest.split_whitespace().collect();
    // f[0] is field 3 (state); utime and stime are fields 14 and 15.
    let ticks = |i: usize| f.get(i).and_then(|v| v.parse::<f64>().ok()).unwrap_or(0.0);
    let hz = unsafe { libc::sysconf(libc::_SC_CLK_TCK) } as f64;
    Ok((ticks(11) + ticks(12)) / hz)
}

#[cfg(target_os = "macos")]
fn task_info(pid: u32) -> io::Result<libc::proc_taskinfo> {
    let mut info: libc::proc_taskinfo = unsafe { std::mem::zeroed() };
    let size = std::mem::size_of::<libc::proc_taskinfo>() as libc::c_int;
    let n = unsafe {
        libc::proc_pidinfo(
            pid as libc::c_int,
            libc::PROC_PIDTASKINFO,
            0,
            &mut info as *mut _ as *mut libc::c_void,
            size,
        )
    };
    if n != size {
        return Err(io::Error::last_os_error());
    }
    Ok(info)
}

/// Cumulative user + system CPU time of `pid`, in seconds.
#[cfg(target_os = "macos")]
#[allow(deprecated)] // libc's mach_timebase_info; the mach2 crate would be one more dependency
pub fn cpu_seconds(pid: u32) -> io::Result<f64> {
    let info = task_info(pid)?;
    // The totals are in Mach absolute time units, which are not nanoseconds on Apple silicon.
    let tb = {
        let mut tb = libc::mach_timebase_info { numer: 0, denom: 0 };
        unsafe { libc::mach_timebase_info(&mut tb) };
        tb
    };
    let ticks = (info.pti_total_user + info.pti_total_system) as f64;
    Ok(ticks * tb.numer as f64 / tb.denom as f64 / 1e9)
}

/// Resident memory of `pid`, in bytes.
#[cfg(target_os = "linux")]
pub fn rss_bytes(pid: u32) -> Option<u64> {
    let statm = std::fs::read_to_string(format!("/proc/{pid}/statm")).ok()?;
    let pages: u64 = statm.split_whitespace().nth(1)?.parse().ok()?;
    Some(pages * unsafe { libc::sysconf(libc::_SC_PAGESIZE) } as u64)
}

#[cfg(target_os = "macos")]
pub fn rss_bytes(pid: u32) -> Option<u64> {
    task_info(pid).ok().map(|i| i.pti_resident_size)
}

/// The executable name of `pid`, to check a recorded PID still is the process it was.
#[cfg(target_os = "linux")]
pub fn name(pid: u32) -> Option<String> {
    std::fs::read_to_string(format!("/proc/{pid}/comm")).ok().map(|s| s.trim().to_string())
}

#[cfg(target_os = "macos")]
pub fn name(pid: u32) -> Option<String> {
    let mut buf = [0u8; 256];
    let n = unsafe {
        libc::proc_name(pid as libc::c_int, buf.as_mut_ptr() as *mut libc::c_void, buf.len() as u32)
    };
    (n > 0).then(|| String::from_utf8_lossy(&buf[..n as usize]).into_owned())
}

pub fn alive(pid: u32) -> bool {
    unsafe { libc::kill(pid as libc::pid_t, 0) == 0 }
}

/// SIGTERM, then SIGKILL after `grace`. Returns once the process is gone.
pub fn terminate(pid: u32, grace: Duration) {
    unsafe { libc::kill(pid as libc::pid_t, libc::SIGTERM) };
    let end = Instant::now() + grace;
    while alive(pid) && Instant::now() < end {
        reap(pid);
        std::thread::sleep(Duration::from_millis(50));
    }
    if alive(pid) {
        unsafe { libc::kill(pid as libc::pid_t, libc::SIGKILL) };
        std::thread::sleep(Duration::from_millis(50));
    }
    reap(pid);
}

/// Collect `pid` if it is our exited child, so `alive` stops seeing a zombie.
fn reap(pid: u32) {
    let mut status = 0;
    unsafe { libc::waitpid(pid as libc::pid_t, &mut status, libc::WNOHANG) };
}

/// Start `cmd` in its own process group with output appended to `log`, so it outlives the
/// harness command that started it (the stack) and Ctrl-C in the terminal doesn't reach it.
pub fn spawn_logged(cmd: &mut Command, log: &Path) -> io::Result<Child> {
    use std::os::unix::process::CommandExt;
    let out = std::fs::OpenOptions::new().create(true).append(true).open(log)?;
    cmd.stdin(Stdio::null())
        .stdout(out.try_clone()?)
        .stderr(out)
        .process_group(0)
        .spawn()
}

/// Kills the processes it holds when dropped, so a failed run leaves nothing behind.
#[derive(Default)]
pub struct Guard(pub Vec<u32>);

impl Guard {
    pub fn stop_all(&mut self) {
        for pid in self.0.drain(..) {
            terminate(pid, Duration::from_secs(5));
        }
    }
}

impl Drop for Guard {
    fn drop(&mut self) {
        self.stop_all();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn measures_this_process() {
        let me = std::process::id();
        let before = cpu_seconds(me).unwrap();
        // Burn some CPU.
        let t = Instant::now();
        let mut x = 0u64;
        while t.elapsed() < Duration::from_millis(200) {
            x = x.wrapping_mul(6364136223846793005).wrapping_add(1);
        }
        std::hint::black_box(x);
        let used = cpu_seconds(me).unwrap() - before;
        assert!((0.1..1.0).contains(&used), "used {used} s of CPU for a 200 ms busy loop");
        assert!(rss_bytes(me).unwrap() > 1_000_000);
        assert!(name(me).is_some());
        assert!(alive(me));
    }
}
