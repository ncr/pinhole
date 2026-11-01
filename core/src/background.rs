//! Work done for the library in the background (reading a folder, making
//! its thumbnails with ImageMagick and ffmpeg) must never be what slows the
//! computer down. Every program it starts runs at the lowest priority there
//! is (SCHED_IDLE, nice 19, which also makes its disk reads the lowest of
//! their class), so anything the person is doing goes first; and only on
//! four in five of the physical cores (with their second threads), so the
//! rest stay wholly free, however many threads a decoder starts of its own
//! (libheif does, for HEIC). As many programs run at once as those cores
//! have threads.

use std::io::Read;
use std::os::unix::process::CommandExt;
use std::path::Path;
use std::process::{Command, Output, Stdio};
use std::sync::{mpsc, Arc, Condvar, Mutex, OnceLock};
use std::time::Duration;

/// How many programs at once: the threads of the cores they may use.
pub fn cores() -> usize {
    allowed().len()
}

/// The CPUs background work may use: every thread of the first four in five
/// physical cores, at least one core. Without the kernel's topology, four in
/// five of the CPUs.
fn allowed() -> &'static [usize] {
    static ALLOWED: OnceLock<Vec<usize>> = OnceLock::new();
    ALLOWED.get_or_init(|| {
        let all = std::thread::available_parallelism().map_or(1, |n| n.get());
        let topology = |cpu: usize, what: &str| -> Option<u32> {
            std::fs::read_to_string(format!("/sys/devices/system/cpu/cpu{cpu}/topology/{what}")).ok()?.trim().parse().ok()
        };
        let core_of: Option<Vec<(u32, u32)>> =
            (0..all).map(|cpu| Some((topology(cpu, "physical_package_id")?, topology(cpu, "core_id")?))).collect();
        match core_of {
            Some(core_of) => {
                let mut cores: Vec<(u32, u32)> = core_of.clone();
                cores.sort();
                cores.dedup();
                let kept = &cores[..(cores.len() * 4 / 5).max(1)];
                (0..all).filter(|cpu| kept.contains(&core_of[*cpu])).collect()
            }
            None => (0..(all * 4 / 5).max(1)).collect(),
        }
    })
}

/// A program to start for background work: idle priority, one thread.
pub fn command(program: &str) -> Command {
    let mut command = Command::new(program);
    // ImageMagick's own threads, one per core unless told.
    command.env("MAGICK_THREAD_LIMIT", "1");
    let mut mask = [0u64; 16];
    for &cpu in allowed().iter().filter(|cpu| **cpu < 1024) {
        mask[cpu / 64] |= 1 << (cpu % 64);
    }
    // SAFETY: only async-signal-safe calls between fork and exec.
    unsafe {
        command.pre_exec(move || {
            lower(&mask);
            Ok(())
        });
    }
    command
}

/// A place among the programs running at once, held while one runs.
pub fn slot() -> Slot {
    Slot::take(&CPU, cores())
}

/// A place among the programs that read a video over the network (a
/// provider's, by URL: render.rs), at most `NETWORK_MAX` at once and apart
/// from `slot`'s. They mostly wait on a server, a minute at a time at
/// worst, and holding one of `slot`'s places meanwhile would hold a
/// folder's thumbnails up behind a slow server. Each decodes on one thread
/// at idle priority, on the same cores.
pub fn network_slot() -> Slot {
    Slot::take(&NETWORK, NETWORK_MAX)
}

/// Programs reading over the network at once.
pub const NETWORK_MAX: usize = 2;

static CPU: Slots = Slots::new();
static NETWORK: Slots = Slots::new();

struct Slots {
    running: Mutex<usize>,
    freed: Condvar,
}

impl Slots {
    const fn new() -> Slots {
        Slots { running: Mutex::new(0), freed: Condvar::new() }
    }
}

pub struct Slot(&'static Slots);

impl Slot {
    fn take(slots: &'static Slots, max: usize) -> Slot {
        let mut running = slots.running.lock().unwrap();
        while *running >= max {
            running = slots.freed.wait(running).unwrap();
        }
        *running += 1;
        Slot(slots)
    }
}

impl Drop for Slot {
    fn drop(&mut self) {
        *self.0.running.lock().unwrap() -= 1;
        self.0.freed.notify_one();
    }
}

/// How long a program making a thumbnail or medium (ImageMagick, ffmpeg,
/// ffprobe) may run before it is stopped: none takes minutes, even at idle
/// priority on a busy computer, unless what it reads no longer answers.
pub const DEADLINE: Duration = Duration::from_secs(180);

/// What a program of background work wrote, once it has ended; or an
/// error, the program killed, when it is still going after `deadline` (a
/// file on a mount that no longer answers). Whoever waits is free then,
/// and the slot it holds with it, even when the program cannot end yet
/// (stuck in the kernel on that mount): it is reaped when it does.
pub fn output(command: &mut Command, deadline: Duration) -> std::io::Result<Output> {
    let mut child = command.stdout(Stdio::piped()).stderr(Stdio::piped()).spawn()?;
    let (mut stdout, mut stderr) = (child.stdout.take().unwrap(), child.stderr.take().unwrap());
    let child = Arc::new(Mutex::new(child));
    let (tx, rx) = mpsc::channel::<std::io::Result<Output>>();
    let waited = child.clone();
    std::thread::spawn(move || {
        let errors = std::thread::spawn(move || {
            let mut err = Vec::new();
            let _ = stderr.read_to_end(&mut err);
            err
        });
        let mut out = Vec::new();
        let read = stdout.read_to_end(&mut out);
        let err = errors.join().unwrap_or_default();
        // Its pipes closed: it has ended, or is about to.
        let status = loop {
            match waited.lock().unwrap().try_wait() {
                Ok(Some(status)) => break Ok(status),
                Ok(None) => std::thread::sleep(Duration::from_millis(2)),
                Err(e) => break Err(e),
            }
        };
        let _ = tx.send(read.and(status).map(|status| Output { status, stdout: out, stderr: err }));
    });
    match rx.recv_timeout(deadline) {
        Ok(output) => output,
        Err(_) => {
            // Killed, not yet reaped: its pid stays its own until the
            // thread above reaps it.
            let _ = child.lock().unwrap().kill();
            let program = Path::new(command.get_program()).file_name().unwrap_or_default().to_string_lossy().into_owned();
            Err(std::io::Error::new(std::io::ErrorKind::TimedOut, format!("{program} still going after {} s: stopped", deadline.as_secs())))
        }
    }
}

// The calls, declared here rather than taking a crate for them.
#[repr(C)]
struct SchedParam {
    priority: i32,
}
extern "C" {
    fn setpriority(which: i32, who: u32, prio: i32) -> i32;
    fn sched_setscheduler(pid: i32, policy: i32, param: *const SchedParam) -> i32;
    fn sched_setaffinity(pid: i32, size: usize, mask: *const u64) -> i32;
}
const PRIO_PROCESS: i32 = 0;
const SCHED_IDLE: i32 = 5;

/// The calling process at the lowest priority, run only when nothing else
/// wants the CPU, and only on the CPUs in `mask`. Failing is harmless: it
/// then just runs as it would have.
fn lower(mask: &[u64; 16]) {
    unsafe {
        setpriority(PRIO_PROCESS, 0, 19);
        sched_setscheduler(0, SCHED_IDLE, &SchedParam { priority: 0 });
        sched_setaffinity(0, std::mem::size_of_val(mask), mask.as_ptr());
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_program_started_for_background_work_runs_idle_and_nice() {
        // /proc/self/stat of the child: field 19 is its nice value, 41 its
        // scheduling policy.
        let out = command("sh").args(["-c", "cat /proc/self/stat"]).output().unwrap();
        let stat = String::from_utf8(out.stdout).unwrap();
        let fields: Vec<&str> = stat.rsplit_once(") ").unwrap().1.split(' ').collect();
        assert_eq!(fields[16], "19", "nice");
        assert_eq!(fields[38], SCHED_IDLE.to_string(), "policy");
    }

    #[test]
    fn it_runs_only_on_four_in_five_cores_with_their_second_threads() {
        let out = command("sh").args(["-c", "grep Cpus_allowed_list /proc/self/status"]).output().unwrap();
        let line = String::from_utf8(out.stdout).unwrap();
        let all = std::thread::available_parallelism().unwrap().get();
        assert!(cores() < all || all < 2, "{line}");
        assert!(cores() >= 1);
        // The CPUs it may use are exactly those it was given.
        let listed: usize = line.split_once(':').unwrap().1.trim().split(',')
            .map(|r| r.split_once('-').map_or(1, |(a, b)| b.parse::<usize>().unwrap() - a.parse::<usize>().unwrap() + 1))
            .sum();
        assert_eq!(listed, cores(), "{line}");
    }

    #[test]
    fn never_more_at_once_than_four_in_five_cores() {
        use std::sync::atomic::{AtomicUsize, Ordering};
        let (now, most) = (AtomicUsize::new(0), AtomicUsize::new(0));
        std::thread::scope(|s| {
            for _ in 0..cores() * 3 {
                s.spawn(|| {
                    let _slot = slot();
                    let n = now.fetch_add(1, Ordering::SeqCst) + 1;
                    most.fetch_max(n, Ordering::SeqCst);
                    std::thread::sleep(std::time::Duration::from_millis(20));
                    now.fetch_sub(1, Ordering::SeqCst);
                });
            }
        });
        assert!(most.load(Ordering::SeqCst) <= cores());
        assert!(cores() >= 1);
    }

    /// Reads over the network take places of their own: every place of
    /// `slot` taken, one is still had at once.
    #[test]
    fn a_program_reading_over_the_network_waits_for_none_of_the_others() {
        let all: Vec<Slot> = (0..cores()).map(|_| slot()).collect();
        let (tx, rx) = mpsc::channel();
        std::thread::spawn(move || {
            let _slot = network_slot();
            let _ = tx.send(());
        });
        assert!(rx.recv_timeout(Duration::from_secs(2)).is_ok());
        drop(all);
    }

    #[test]
    fn a_program_still_going_after_its_deadline_is_stopped_and_reaped() {
        let dir = std::env::temp_dir().join(format!("pinhole-deadline-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let pid_file = dir.join("pid");
        let started = std::time::Instant::now();
        let mut sleeping = command("sh");
        sleeping.arg("-c").arg(format!("echo $$ > {}; exec sleep 60", pid_file.display()));
        let e = output(&mut sleeping, Duration::from_millis(500)).unwrap_err();
        assert_eq!(e.kind(), std::io::ErrorKind::TimedOut, "{e}");
        assert!(started.elapsed() < Duration::from_secs(5), "{:?}", started.elapsed());
        let pid = std::fs::read_to_string(&pid_file).unwrap().trim().to_owned();
        let gone = std::time::Instant::now();
        while Path::new("/proc").join(&pid).exists() && gone.elapsed() < Duration::from_secs(5) {
            std::thread::sleep(Duration::from_millis(20));
        }
        assert!(!Path::new("/proc").join(&pid).exists(), "{pid} not reaped");
        // One that ends in time: what it wrote, and how it ended.
        let out = output(command("sh").args(["-c", "echo out; echo err >&2; exit 3"]), Duration::from_secs(10)).unwrap();
        assert_eq!((&out.stdout[..], &out.stderr[..], out.status.code()), (&b"out\n"[..], &b"err\n"[..], Some(3)));
        let _ = std::fs::remove_dir_all(&dir);
    }
}
