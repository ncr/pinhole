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
use std::time::{Duration, Instant};

/// How many programs at once: the selected threads, capped by the CPU quota.
pub fn cores() -> usize {
    static SLOTS: OnceLock<usize> = OnceLock::new();
    *SLOTS.get_or_init(|| {
        let budget = std::thread::available_parallelism().map_or(1, |n| n.get());
        allowed().len().clamp(1, budget)
    })
}

/// The CPUs background work may use: every thread of the first four in five
/// physical cores, at least one core. Without the kernel's topology, four in
/// five of the CPUs.
fn allowed() -> &'static [usize] {
    static ALLOWED: OnceLock<Vec<usize>> = OnceLock::new();
    ALLOWED.get_or_init(|| {
        // A cgroup CPU quota is a count, and an inherited affinity mask
        // can contain nonconsecutive CPU IDs.
        let mut mask = [0u64; 16];
        if unsafe { sched_getaffinity(0, std::mem::size_of_val(&mask), mask.as_mut_ptr()) } != 0 {
            return Vec::new();
        }
        let cpus: Vec<_> = (0..mask.len() * 64).filter(|cpu| mask[cpu / 64] & (1 << (cpu % 64)) != 0).collect();
        let topology = |cpu| {
            let read = |what| {
                std::fs::read_to_string(format!("/sys/devices/system/cpu/cpu{cpu}/topology/{what}")).ok()?.trim().parse().ok()
            };
            Some((read("physical_package_id")?, read("core_id")?))
        };
        background_cpus(&cpus, topology)
    })
}

fn background_cpus(cpus: &[usize], topology: impl Fn(usize) -> Option<(u32, u32)>) -> Vec<usize> {
    let core_of: Option<Vec<_>> = cpus.iter().copied().map(topology).collect();
    let Some(core_of) = core_of else {
        return cpus.iter().copied().take((cpus.len() * 4 / 5).max(1)).collect();
    };
    let mut cores = core_of.clone();
    cores.sort_unstable();
    cores.dedup();
    let kept = &cores[..(cores.len() * 4 / 5).max(1).min(cores.len())];
    cpus.iter().copied().zip(core_of).filter_map(|(cpu, core)| kept.contains(&core).then_some(cpu)).collect()
}

/// A program to start for background work: idle priority, one thread.
pub fn command(program: &str) -> Command {
    let mut command = Command::new(program);
    // ImageMagick's own threads, one per core unless told.
    command.env("MAGICK_THREAD_LIMIT", "1");
    // A process group of its own, so that whatever it starts is stopped
    // with it (`output`).
    command.process_group(0);
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

/// When a program of background work (ImageMagick, ffmpeg, ffprobe) is
/// stopped. Never for taking long: at idle priority it gets a processor
/// only when nothing else wants one, so on a computer busy with a build it
/// may wait minutes for its first slice, which says nothing about what it
/// reads. It is stopped when it is
///
/// - hung: for `quiet` it used no processor time, read and wrote nothing,
///   and no thread of it was waiting for a processor (state R, as a
///   starved one is), nor did any program it started do any of that. It
///   sleeps (S) or waits in the kernel (D) on something that does not
///   answer: a file on a mount that has gone, a FIFO with no writer. A
///   slow mount still reads now and then, which is progress. A read held
///   two minutes by a busy disk would look the same, but its idle priority
///   counts only under BFQ, and NVMe disks go without a scheduler;
/// - or spinning: it has used `cpu` of processor time, which no thumbnail
///   or medium takes (a frame of a 4K HEVC video takes seconds), and a
///   file sending a decoder round in circles does.
///
/// It is looked at `every` so often: its /proc/<pid>/stat (processor time)
/// and io (bytes read and written); its threads' states only when those
/// have not moved, and the programs it started (its process group) only
/// when no thread of it is waiting for a processor either. One making
/// progress costs two small reads, a starved one a few more.
pub struct Limits {
    pub quiet: Duration,
    pub cpu: Duration,
    pub every: Duration,
}

pub const LIMITS: Limits =
    Limits { quiet: Duration::from_secs(120), cpu: Duration::from_secs(300), every: Duration::from_secs(2) };

/// What a program of background work wrote, once it has ended; or an
/// error of the kind `TimedOut`, it and whatever it started killed, when it
/// is hung or spinning (`LIMITS`). Whoever waits is free then, and the slot
/// it holds with it, even when the program cannot end yet (stuck in the
/// kernel on a mount that has gone): it is reaped when it does.
pub fn output(command: &mut Command) -> std::io::Result<Output> {
    output_within(command, &LIMITS)
}

/// The programs `output` is running now, by pid: each its own process
/// group's id while unreaped (a pid is taken out of here as it is reaped,
/// under this lock), so `stop_all` never stops another's.
static PROGRAMS: Mutex<Vec<u32>> = Mutex::new(Vec::new());

/// Stop every program `output` is running, and whatever each started: a
/// service ending (one of a library given) leaves none behind to go on
/// writing into the cache. Those waiting on them hear an error.
pub fn stop_all() {
    let programs = PROGRAMS.lock().unwrap_or_else(|e| e.into_inner());
    for &pid in programs.iter() {
        unsafe { kill(-(pid as i32), SIGKILL) };
        unsafe { kill(pid as i32, SIGKILL) };
    }
}

/// The same as `output`, within `limits`.
pub fn output_within(command: &mut Command, limits: &Limits) -> std::io::Result<Output> {
    let mut programs = PROGRAMS.lock().unwrap_or_else(|e| e.into_inner());
    let mut child = command.stdout(Stdio::piped()).stderr(Stdio::piped()).spawn()?;
    let pid = child.id();
    programs.push(pid);
    drop(programs);
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
            let mut programs = PROGRAMS.lock().unwrap_or_else(|e| e.into_inner());
            let status = waited.lock().unwrap().try_wait();
            if !matches!(status, Ok(None)) {
                programs.retain(|p| *p != pid);
            }
            drop(programs);
            match status {
                Ok(Some(status)) => break Ok(status),
                Ok(None) => std::thread::sleep(Duration::from_millis(2)),
                Err(e) => break Err(e),
            }
        };
        let _ = tx.send(read.and(status).map(|status| Output { status, stdout: out, stderr: err }));
    });
    let mut watching = Watching::new(Instant::now());
    let stop = loop {
        match rx.recv_timeout(limits.every) {
            Ok(output) => return output,
            Err(mpsc::RecvTimeoutError::Disconnected) => return Err(std::io::Error::other("its reader ended")),
            Err(mpsc::RecvTimeoutError::Timeout) => {}
        }
        if let Some(stop) = watching.step(Instant::now(), Seen::of(pid), || runnable(pid), || group(pid), limits) {
            break stop;
        }
    };
    {
        // Its process group is its own (`command`): whatever it started
        // goes with it, even after it has ended itself. The group's id is
        // its pid, which is not another's while it is unreaped (the thread
        // above reaps it only under this lock) or any of the group is left,
        // and pids are handed out in turn, so not again for a long while.
        let mut child = child.lock().unwrap();
        unsafe { kill(-(pid as i32), SIGKILL) };
        let _ = child.kill();
    }
    let program = Path::new(command.get_program()).file_name().unwrap_or_default().to_string_lossy().into_owned();
    let why = match stop {
        Stop::Hung => format!("{program} did nothing for {} s: stopped", limits.quiet.as_secs()),
        Stop::Spinning => format!("{program} used {} s of processor time: stopped", limits.cpu.as_secs()),
    };
    Err(std::io::Error::new(std::io::ErrorKind::TimedOut, Stopped { stop, why }))
}

/// Why a program was stopped (`Limits`).
#[derive(Debug, PartialEq)]
enum Stop {
    Hung,
    Spinning,
}

/// What `output` says of a program it stopped, inside its `TimedOut`.
#[derive(Debug)]
struct Stopped {
    stop: Stop,
    why: String,
}

impl std::fmt::Display for Stopped {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(&self.why)
    }
}

impl std::error::Error for Stopped {}

/// Whether `output` stopped a program for spinning: something in what it
/// read sent it round in circles, which says that the input is bad. One
/// hung says nothing of it, only that what it reads does not answer.
pub fn spun(e: &std::io::Error) -> bool {
    e.get_ref().and_then(|e| e.downcast_ref::<Stopped>()).is_some_and(|s| s.stop == Stop::Spinning)
}

/// What a process has done so far: processor time, in clock ticks, and
/// bytes read and written (a pipe's too).
#[derive(Clone, Copy, Debug, Default, PartialEq)]
struct Seen {
    ticks: u64,
    bytes: u64,
}

impl Seen {
    /// Of the process `pid`, all its threads; nothing when it is gone.
    fn of(pid: u32) -> Seen {
        let stat = std::fs::read_to_string(format!("/proc/{pid}/stat")).unwrap_or_default();
        // utime and stime: fields 14 and 15, the 12th and 13th after the
        // name, which may hold anything but ends with ") ".
        let after = stat.rsplit_once(") ").map_or("", |(_, after)| after);
        let ticks = after.split(' ').skip(11).take(2).filter_map(|t| t.parse::<u64>().ok()).sum();
        let io = std::fs::read_to_string(format!("/proc/{pid}/io")).unwrap_or_default();
        let bytes = io
            .lines()
            .filter_map(|line| line.strip_prefix("rchar: ").or_else(|| line.strip_prefix("wchar: ")))
            .filter_map(|n| n.trim().parse::<u64>().ok())
            .sum();
        Seen { ticks, bytes }
    }
}

/// Whether any thread of the process `pid` is running or waiting for a
/// processor (R).
fn runnable(pid: u32) -> bool {
    let Ok(tasks) = std::fs::read_dir(format!("/proc/{pid}/task")) else { return false };
    tasks.flatten().any(|task| {
        let stat = std::fs::read_to_string(task.path().join("stat")).unwrap_or_default();
        stat.rsplit_once(") ").is_some_and(|(_, after)| after.starts_with('R'))
    })
}

/// What the other processes of the process group `pgid` have done, all
/// together, and whether a thread of any is waiting for a processor.
fn group(pgid: u32) -> (Seen, bool) {
    let mut all = (Seen::default(), false);
    let Ok(procs) = std::fs::read_dir("/proc") else { return all };
    let pgrp = pgid.to_string();
    for pid in procs.flatten().filter_map(|p| p.file_name().to_str()?.parse::<u32>().ok()) {
        if pid == pgid {
            continue;
        }
        // pgrp: field 5, the 3rd after the name.
        let stat = std::fs::read_to_string(format!("/proc/{pid}/stat")).unwrap_or_default();
        if stat.rsplit_once(") ").and_then(|(_, after)| after.split(' ').nth(2)) != Some(pgrp.as_str()) {
            continue;
        }
        let seen = Seen::of(pid);
        all.0.ticks += seen.ticks;
        all.0.bytes += seen.bytes;
        all.1 |= runnable(pid);
    }
    all
}

/// A program watched (`Limits`): what it and the programs it started had
/// done when last looked at, and since when none has done anything.
struct Watching {
    seen: Seen,
    others: Option<Seen>,
    since: Instant,
}

impl Watching {
    fn new(now: Instant) -> Watching {
        Watching { seen: Seen::default(), others: None, since: now }
    }

    /// Looked at again `now`, having done `seen`: whether to stop it. Its
    /// threads (`runnable`), and then the programs it started (`group`),
    /// are looked at only when it has done nothing since.
    fn step(
        &mut self,
        now: Instant,
        seen: Seen,
        runnable: impl FnOnce() -> bool,
        group: impl FnOnce() -> (Seen, bool),
        limits: &Limits,
    ) -> Option<Stop> {
        if seen.ticks >= limits.cpu.as_secs() * clock_ticks() {
            return Some(Stop::Spinning);
        }
        let going = seen != self.seen || runnable() || {
            let (others, waiting) = group();
            let moved = self.others.is_some_and(|before| before != others);
            self.others = Some(others);
            moved || waiting
        };
        self.seen = seen;
        if going {
            self.since = now;
        }
        (now.duration_since(self.since) >= limits.quiet).then_some(Stop::Hung)
    }
}

/// Clock ticks a second, as /proc counts processor time.
fn clock_ticks() -> u64 {
    static TICKS: OnceLock<u64> = OnceLock::new();
    *TICKS.get_or_init(|| match unsafe { sysconf(SC_CLK_TCK) } {
        n if n > 0 => n as u64,
        _ => 100,
    })
}

// The calls, declared here rather than taking a crate for them.
#[repr(C)]
struct SchedParam {
    priority: i32,
}
extern "C" {
    fn setpriority(which: i32, who: u32, prio: i32) -> i32;
    fn sched_setscheduler(pid: i32, policy: i32, param: *const SchedParam) -> i32;
    fn sched_getaffinity(pid: i32, size: usize, mask: *mut u64) -> i32;
    fn sched_setaffinity(pid: i32, size: usize, mask: *const u64) -> i32;
    fn kill(pid: i32, signal: i32) -> i32;
    fn sysconf(name: i32) -> i64;
}
const SIGKILL: i32 = 9;
const SC_CLK_TCK: i32 = 2;
const PRIO_PROCESS: i32 = 0;
const SCHED_IDLE: i32 = 5;

/// The calling process at the lowest priority, run only when nothing else
/// wants the CPU, and only on the CPUs in `mask`. Failing is harmless: it
/// then just runs as it would have.
fn lower(mask: &[u64; 16]) {
    unsafe {
        setpriority(PRIO_PROCESS, 0, 19);
        sched_setscheduler(0, SCHED_IDLE, &SchedParam { priority: 0 });
        if mask.iter().any(|word| *word != 0) {
            sched_setaffinity(0, std::mem::size_of_val(mask), mask.as_ptr());
        }
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
    fn background_cpus_keep_siblings_with_nonconsecutive_ids() {
        let cpus = [2, 3, 4, 5, 6, 10, 11, 12, 13, 14];
        let siblings = |cpu| Some((0, cpu as u32 % 8));
        assert_eq!(background_cpus(&cpus, siblings), [2, 3, 4, 5, 10, 11, 12, 13]);
        assert_eq!(background_cpus(&[2, 10], siblings), [2, 10]);
        assert_eq!(background_cpus(&[4, 8, 12], |_| None), [4, 8]);
    }

    #[test]
    fn a_background_program_inherits_only_the_selected_cpus() {
        let out = command("sh").args(["-c", "grep Cpus_allowed_list /proc/self/status"]).output().unwrap();
        let line = String::from_utf8(out.stdout).unwrap();
        let all = std::thread::available_parallelism().unwrap().get();
        assert!((1..=all).contains(&cores()));
        let listed: Vec<_> = line.split_once(':').unwrap().1.trim().split(',').flat_map(|part| {
            let (first, last) = part.split_once('-').unwrap_or((part, part));
            first.parse::<usize>().unwrap()..=last.parse::<usize>().unwrap()
        }).collect();
        assert_eq!(listed, allowed(), "{line}");
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

    /// Limits short enough for a test.
    fn quick(quiet: Duration) -> Limits {
        Limits { quiet, cpu: Duration::from_secs(60), every: Duration::from_millis(100) }
    }

    fn gone(pid: &str) -> bool {
        let since = Instant::now();
        // Gone, or a zombie its new parent has yet to reap.
        let alive = || std::fs::read_to_string(format!("/proc/{pid}/stat")).is_ok_and(|s| !s.contains(") Z "));
        while alive() && since.elapsed() < Duration::from_secs(5) {
            std::thread::sleep(Duration::from_millis(20));
        }
        !alive()
    }

    /// One asleep for good, or waiting on a FIFO no one writes to, with a
    /// program it started: stopped once it has done nothing for `quiet`,
    /// and both are gone.
    #[test]
    fn a_hung_program_is_stopped_with_what_it_started() {
        let dir = std::env::temp_dir().join(format!("pinhole-hung-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        let quiet = Duration::from_secs(1);

        let pid_file = dir.join("pid");
        let started = Instant::now();
        let mut sleeping = command("sh");
        sleeping.arg("-c").arg(format!("echo $$ > {}; exec sleep 60", pid_file.display()));
        let e = output_within(&mut sleeping, &quick(quiet)).unwrap_err();
        assert_eq!(e.kind(), std::io::ErrorKind::TimedOut, "{e}");
        assert!(e.to_string().contains("did nothing for 1 s"), "{e}");
        assert!(!spun(&e));
        assert!(started.elapsed() >= quiet && started.elapsed() < Duration::from_secs(5), "{:?}", started.elapsed());
        let pid = std::fs::read_to_string(&pid_file).unwrap().trim().to_owned();
        assert!(gone(&pid), "{pid} not reaped");

        let fifo = dir.join("fifo");
        assert!(Command::new("mkfifo").arg(&fifo).status().unwrap().success());
        let reader = dir.join("reader");
        let mut waiting = command("sh");
        waiting.arg("-c").arg(format!("echo $$ > {}; cat {} & echo $! > {}; wait", pid_file.display(), fifo.display(), reader.display()));
        let started = Instant::now();
        let e = output_within(&mut waiting, &quick(quiet)).unwrap_err();
        assert_eq!(e.kind(), std::io::ErrorKind::TimedOut, "{e}");
        assert!(started.elapsed() >= quiet && started.elapsed() < Duration::from_secs(5), "{:?}", started.elapsed());
        let (sh, cat) = (std::fs::read_to_string(&pid_file).unwrap(), std::fs::read_to_string(&reader).unwrap());
        assert!(gone(sh.trim()), "sh {sh} still there");
        assert!(gone(cat.trim()), "cat {cat} still there");

        // One that ends: what it wrote, and how it ended.
        let out = output(command("sh").args(["-c", "echo out; echo err >&2; exit 3"])).unwrap();
        assert_eq!((&out.stdout[..], &out.stderr[..], out.status.code()), (&b"out\n"[..], &b"err\n"[..], Some(3)));
        let _ = std::fs::remove_dir_all(&dir);
    }

    /// A busy program at normal priority on the one CPU it may use: it
    /// waits for a processor (R) many times as long as it may do nothing,
    /// is not stopped, and ends once that one is done.
    #[test]
    fn a_program_starved_of_the_processor_is_not_stopped() {
        if Command::new("taskset").arg("-V").stdout(Stdio::null()).status().is_err() {
            return;
        }
        let cpu = allowed()[0];
        let load = Duration::from_secs(12);
        let (busy_tx, busy_rx) = mpsc::channel();
        let busy = std::thread::spawn(move || {
            let mut mask = [0u64; 16];
            mask[cpu / 64] |= 1 << (cpu % 64);
            unsafe { sched_setaffinity(0, std::mem::size_of_val(&mask), mask.as_ptr()) };
            busy_tx.send(()).unwrap();
            let since = Instant::now();
            let mut n = 0u64;
            while since.elapsed() < load {
                n = std::hint::black_box(n.wrapping_add(1));
            }
        });
        busy_rx.recv().unwrap();
        let started = Instant::now();
        // Half a second's work, at idle priority, on that CPU alone.
        let mut starved = command("taskset");
        starved.args(["-c", &cpu.to_string(), "sh", "-c", "i=0; while [ $i -lt 300000 ]; do i=$((i+1)); done; echo done"]);
        let out = output_within(&mut starved, &quick(Duration::from_secs(2))).unwrap();
        assert_eq!(&out.stdout[..], b"done\n", "{}", String::from_utf8_lossy(&out.stderr));
        // Whether it was starved is the scheduler's to say: the kernel's
        // own gives an idle program next to nothing beside a busy one, but
        // another (a sched_ext one, say) may give it a share, and then it
        // simply ends. Not stopped either way; starved, only noted.
        if started.elapsed() < load - Duration::from_secs(2) {
            eprintln!("this scheduler gave the idle program time ({:?}): starvation not exercised", started.elapsed());
        }
        busy.join().unwrap();
    }

    /// One waiting on a program it started (ImageMagick on a delegate),
    /// which works: not stopped.
    #[test]
    fn a_program_waiting_on_one_it_started_that_works_is_not_stopped() {
        // Longer than the quiet it is given, the one waiting doing nothing
        // all along. A second of quiet, not less: on a computer busy with
        // every core (bin/test-all), an idle-priority child can go a few
        // hundred milliseconds without a look-in.
        let quiet = Duration::from_secs(1);
        let mut waiting = command("sh");
        waiting.args(["-c", "(i=0; while [ $i -lt 2000000 ]; do i=$((i+1)); done); echo done"]);
        let started = Instant::now();
        let out = output_within(&mut waiting, &quick(quiet)).unwrap();
        assert_eq!(&out.stdout[..], b"done\n");
        assert!(started.elapsed() > quiet, "{:?}", started.elapsed());
    }

    #[test]
    fn a_program_spinning_is_stopped_once_it_has_used_its_processor_time() {
        let limits = Limits { quiet: Duration::from_secs(60), cpu: Duration::from_secs(1), every: Duration::from_millis(100) };
        let e = output_within(command("sh").args(["-c", "while :; do :; done"]), &limits).unwrap_err();
        assert_eq!(e.kind(), std::io::ErrorKind::TimedOut, "{e}");
        assert!(e.to_string().contains("used 1 s of processor time"), "{e}");
        assert!(spun(&e));
    }

    /// What is done when it is looked at: a starved program (R, nothing
    /// done) is never hung; one asleep is, after `quiet`, unless a program
    /// it started does something.
    #[test]
    fn hung_is_nothing_done_and_no_processor_waited_for() {
        let limits = Limits { quiet: Duration::from_secs(120), cpu: Duration::from_secs(300), every: Duration::from_secs(2) };
        let start = Instant::now();
        let at = |s: u64| start + Duration::from_secs(s);
        let seen = Seen { ticks: 3, bytes: 4096 };
        let others = (Seen::default(), false);

        let mut starved = Watching::new(start);
        for s in (2..=600).step_by(2) {
            assert_eq!(starved.step(at(s), seen, || true, || panic!("not looked at"), &limits), None, "{s}");
        }

        let mut asleep = Watching::new(start);
        assert_eq!(asleep.step(at(2), seen, || false, || others, &limits), None);
        assert_eq!(asleep.step(at(120), seen, || false, || others, &limits), None);
        assert_eq!(asleep.step(at(122), seen, || false, || others, &limits), Some(Stop::Hung));

        let mut waiting = Watching::new(start);
        for s in (2..=600).step_by(2) {
            let working = (Seen { ticks: s, bytes: 0 }, false);
            assert_eq!(waiting.step(at(s), seen, || false, || working, &limits), None, "{s}");
        }
        let mut waiting_for_cpu = Watching::new(start);
        for s in (2..=600).step_by(2) {
            assert_eq!(waiting_for_cpu.step(at(s), seen, || false, || (Seen::default(), true), &limits), None, "{s}");
        }

        let mut reading = Watching::new(start);
        for s in (2..=600).step_by(2) {
            assert_eq!(reading.step(at(s), Seen { ticks: 3, bytes: s }, || false, || others, &limits), None, "{s}");
        }
        assert_eq!(reading.step(at(800), Seen { ticks: 3, bytes: 600 }, || false, || others, &limits), Some(Stop::Hung));

        let mut spinning = Watching::new(start);
        let spent = Seen { ticks: 300 * clock_ticks(), bytes: 0 };
        assert_eq!(spinning.step(at(2), spent, || true, || others, &limits), Some(Stop::Spinning));
    }
}
