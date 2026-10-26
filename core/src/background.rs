//! Work done for the library in the background (reading a folder, making
//! its thumbnails with ImageMagick and ffmpeg) must never be what slows the
//! computer down. Every program it starts runs at the lowest priority there
//! is (SCHED_IDLE, nice 19, which also makes its disk reads the lowest of
//! their class), so anything the person is doing goes first; and only on
//! four in five of the physical cores (with their second threads), so the
//! rest stay wholly free, however many threads a decoder starts of its own
//! (libheif does, for HEIC). As many programs run at once as those cores
//! have threads.

use std::os::unix::process::CommandExt;
use std::process::Command;
use std::sync::{Condvar, Mutex, OnceLock};

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
    let (count, freed) = slots();
    let mut running = count.lock().unwrap();
    while *running >= cores() {
        running = freed.wait(running).unwrap();
    }
    *running += 1;
    Slot
}

pub struct Slot;

impl Drop for Slot {
    fn drop(&mut self) {
        let (count, freed) = slots();
        *count.lock().unwrap() -= 1;
        freed.notify_one();
    }
}

fn slots() -> &'static (Mutex<usize>, Condvar) {
    static SLOTS: OnceLock<(Mutex<usize>, Condvar)> = OnceLock::new();
    SLOTS.get_or_init(|| (Mutex::new(0), Condvar::new()))
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
}
