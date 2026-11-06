//! The plugins' workers (docs/plugins.md, Workers): programs a plugin's
//! manifest declares under `workers`, which the shared service keeps
//! running for the source shown while the plugin is on. Each is started
//! again when the source shown or the worker changes, ten seconds after it
//! ended by itself, and stopped by closing its stdin: killed if it is
//! still there five seconds later. Its stdout and stderr go into
//! pinholed's log after its plugin's id. A service of a library given
//! (`--given`) runs none.

use std::os::unix::fs::DirBuilderExt;
use std::os::unix::process::{CommandExt, ExitStatusExt};
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::sync::Mutex;
use std::time::{Duration, Instant, SystemTime};

use serde_json::{json, Value};

use crate::plugins::{Places, Worker};

/// How long after a worker ended by itself (or could not start) it is
/// started again.
const RETRY: Duration = Duration::from_secs(10);
/// How long a worker has to end once its stdin closes.
const GRACE: Duration = Duration::from_secs(5);
/// How often the workers wanted are looked at.
const ROUND: Duration = Duration::from_secs(2);
/// How much lower than pinholed's own a worker's CPU priority is.
const NICE: i32 = 10;

/// The source shown, as the workers are given it.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Shown {
    /// Its library's folder (the one with library.sqlite).
    pub library: PathBuf,
    /// The source's id, the library's `meta.source`.
    pub key: String,
}

/// What a worker is started with: run again when that changes.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Job {
    pub program: PathBuf,
    pub args: Vec<String>,
    pub library: PathBuf,
    pub key: String,
    pub cache: PathBuf,
}

impl Job {
    /// Everything it is run with after its program.
    pub fn argv(&self, plugins: &Path) -> Vec<std::ffi::OsString> {
        let mut argv: Vec<std::ffi::OsString> = self.args.iter().map(Into::into).collect();
        argv.extend(["--library".into(), self.library.clone().into()]);
        argv.extend(["--library-key".into(), self.key.clone().into()]);
        argv.extend(["--cache-dir".into(), self.cache.clone().into()]);
        argv.extend(["--managed-plugins".into(), plugins.to_owned().into()]);
        argv
    }
}

/// How a worker last ended.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Exit {
    /// Its exit code, if it exited.
    pub code: Option<i32>,
    /// The signal that ended it, if one did.
    pub signal: Option<i32>,
    /// When, in milliseconds since 1970.
    pub at: u64,
}

/// One worker of a plugin that is on, as the `workers` method gives it.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Status {
    pub plugin: String,
    pub name: String,
    /// What it runs for, or would: none while no source with a library is
    /// shown.
    pub shown: Option<Shown>,
    pub running: bool,
    /// Since when it has been running, or not, in milliseconds since 1970.
    pub since: u64,
    pub last_exit: Option<Exit>,
}

impl Status {
    fn json(&self) -> Value {
        json!({
            "plugin": self.plugin,
            "name": self.name,
            "library": self.shown.as_ref().map(|s| &s.library),
            "key": self.shown.as_ref().map(|s| &s.key),
            "running": self.running,
            "since": self.since,
            "last_exit": self.last_exit.as_ref().map(|e| json!({"code": e.code, "signal": e.signal, "at": e.at})),
        })
    }
}

/// The plugins' workers, and how each is doing.
pub struct Workers {
    /// Where the plugins are; none, and there are no workers.
    places: Option<Places>,
    /// plugins.json: the switches, and given to every worker.
    plugins: PathBuf,
    /// `~/.cache/pinhole/plugins`: a folder in it for each plugin, and in
    /// that one for each source's library, as the window's
    /// `pinhole.cacheDir` (PluginHost::cacheDir).
    cache: PathBuf,
    status: Mutex<Vec<Status>>,
    /// Why any declared were refused, as last said in the log.
    refused: Mutex<Vec<String>>,
}

/// A worker wanted, and its process if it runs.
struct Slot {
    plugin: String,
    name: String,
    job: Option<Job>,
    child: Option<(Job, Child)>,
    retry: Instant,
    since: SystemTime,
    last_exit: Option<Exit>,
}

fn millis(time: SystemTime) -> u64 {
    time.duration_since(SystemTime::UNIX_EPOCH).map_or(0, |d| d.as_millis() as u64)
}

/// `$XDG_CACHE_HOME/pinhole/plugins`.
pub fn cache_root() -> PathBuf {
    std::env::var_os("XDG_CACHE_HOME")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from(std::env::var_os("HOME").unwrap_or_default()).join(".cache"))
        .join("pinhole/plugins")
}

/// A library's folder in a plugin's cache, as the window names it
/// (Plugins::cacheName): library/icloud/<apple id> is icloud-<apple id>.
pub fn cache_name(library: &Path) -> String {
    let name = |p: Option<&Path>| p.and_then(Path::file_name).map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
    format!("{}-{}", name(library.parent()), name(Some(library)))
}

impl Workers {
    pub fn new(places: Option<Places>, plugins: PathBuf, cache: PathBuf) -> Workers {
        Workers { places, plugins, cache, status: Mutex::default(), refused: Mutex::default() }
    }

    /// The `workers` event: as the last round left them.
    pub fn report(&self) -> Value {
        let status = self.status.lock().unwrap_or_else(|e| e.into_inner());
        json!({"workers": status.iter().map(Status::json).collect::<Vec<_>>()})
    }

    /// The `workers` method's answer: the workers wanted now, for the
    /// source shown now, not only as of the last round (two seconds ago at
    /// most), so that a plugin just turned on, or another source just
    /// shown, is answered as it is. Whether each runs, as the last round
    /// left it.
    pub fn report_now(&self, shown: Option<&Shown>) -> Value {
        let status = self.status.lock().unwrap_or_else(|e| e.into_inner()).clone();
        let workers: Vec<Value> = self
            .wanted(shown)
            .into_iter()
            .map(|(w, job)| {
                let last = status.iter().find(|s| s.plugin == w.plugin && s.name == w.name);
                let shown = job.map(|j| Shown { library: j.library, key: j.key });
                // Running for what it is wanted for now, else not (yet).
                let running = last.is_some_and(|s| s.running && s.shown == shown);
                Status {
                    plugin: w.plugin,
                    name: w.name,
                    since: last.filter(|s| s.running == running).map_or_else(|| millis(SystemTime::now()), |s| s.since),
                    last_exit: last.and_then(|s| s.last_exit.clone()),
                    shown,
                    running,
                }
                .json()
            })
            .collect();
        json!({"workers": workers})
    }

    /// The workers of the plugins that are on, each with what it would be
    /// run with for `shown`.
    pub fn wanted(&self, shown: Option<&Shown>) -> Vec<(Worker, Option<Job>)> {
        let Some(places) = &self.places else { return Vec::new() };
        let (found, refused) = crate::plugins::workers(places);
        {
            let mut said = self.refused.lock().unwrap_or_else(|e| e.into_inner());
            for why in refused.iter().filter(|why| !said.contains(why)) {
                eprintln!("pinholed: a worker refused: {why}");
            }
            *said = refused;
        }
        let switches = crate::plugins::switches_in(&self.plugins);
        let builtins = crate::plugins::builtin_ids(places);
        found
            .into_iter()
            .filter(|w| crate::plugins::is_on(&switches, &w.plugin, w.builtin, !w.builtin && builtins.contains(&w.plugin)))
            .map(|w| {
                let job = shown.map(|s| Job {
                    program: w.program.clone(),
                    args: w.args.clone(),
                    library: s.library.clone(),
                    key: s.key.clone(),
                    cache: self.cache.join(&w.plugin).join(cache_name(&s.library)),
                });
                (w, job)
            })
            .collect()
    }

    /// Keep the workers wanted running for the source `shown` says, for as
    /// long as pinholed runs; `changed` is told of every change in how
    /// they are doing, with the `workers` event. Only the shared service
    /// does this.
    pub fn supervise(&self, shown: impl Fn() -> Option<Shown>, changed: impl Fn(Value)) {
        let mut slots: Vec<Slot> = Vec::new();
        loop {
            self.round(&mut slots, shown(), &changed);
            std::thread::sleep(ROUND);
        }
    }

    fn round(&self, slots: &mut Vec<Slot>, shown: Option<Shown>, changed: &impl Fn(Value)) {
        let wanted = self.wanted(shown.as_ref());
        let now = Instant::now();
        // Those of a plugin turned off, gone, or whose manifest no longer
        // has them: stopped, and no longer told of.
        let mut stopping: Vec<Child> = Vec::new();
        slots.retain_mut(|slot| {
            let kept = wanted.iter().any(|(w, _)| w.plugin == slot.plugin && w.name == slot.name);
            if !kept {
                if let Some((_, child)) = slot.child.take() {
                    stopping.push(child);
                }
            }
            kept
        });
        for (worker, job) in wanted {
            let slot = match slots.iter_mut().position(|s| s.plugin == worker.plugin && s.name == worker.name) {
                Some(at) => &mut slots[at],
                None => {
                    slots.push(Slot {
                        plugin: worker.plugin.clone(),
                        name: worker.name.clone(),
                        job: None,
                        child: None,
                        retry: now,
                        since: SystemTime::now(),
                        last_exit: None,
                    });
                    slots.last_mut().unwrap()
                }
            };
            slot.job = job;
        }
        for slot in slots.iter_mut() {
            if slot.child.as_ref().is_some_and(|(job, _)| slot.job.as_ref() != Some(job)) {
                // Another source shown, or the worker changed: started
                // again at once for what it is now.
                let (_, child) = slot.child.take().unwrap();
                stopping.push(child);
                slot.since = SystemTime::now();
                slot.retry = now;
            }
        }
        // Those that ended by themselves.
        for slot in slots.iter_mut() {
            let Some((_, child)) = slot.child.as_mut() else { continue };
            let ended = match child.try_wait() {
                Ok(None) => continue,
                Ok(Some(status)) => Exit { code: status.code(), signal: status.signal(), at: millis(SystemTime::now()) },
                Err(_) => Exit { code: None, signal: None, at: millis(SystemTime::now()) },
            };
            eprintln!("pinholed: {} {}: ended ({})", slot.plugin, slot.name, describe(&ended));
            slot.child = None;
            slot.last_exit = Some(ended);
            slot.since = SystemTime::now();
            slot.retry = now + RETRY;
        }
        stop(stopping);
        for slot in slots.iter_mut().filter(|s| s.child.is_none() && now >= s.retry) {
            let Some(job) = slot.job.clone() else { continue };
            match self.spawn(&slot.plugin, &slot.name, &job) {
                Ok(child) => {
                    slot.child = Some((job, child));
                    slot.since = SystemTime::now();
                }
                Err(error) => {
                    eprintln!("pinholed: {} {}: could not start {}: {error}", slot.plugin, slot.name, job.program.display());
                    slot.retry = now + RETRY;
                }
            }
        }
        let status: Vec<Status> = slots
            .iter()
            .map(|s| Status {
                plugin: s.plugin.clone(),
                name: s.name.clone(),
                shown: s.job.as_ref().map(|j| Shown { library: j.library.clone(), key: j.key.clone() }),
                running: s.child.is_some(),
                since: millis(s.since),
                last_exit: s.last_exit.clone(),
            })
            .collect();
        let mut current = self.status.lock().unwrap_or_else(|e| e.into_inner());
        if *current != status {
            *current = status;
            drop(current);
            let mut event = self.report();
            event["event"] = json!("workers");
            changed(event);
        }
    }

    fn spawn(&self, plugin: &str, name: &str, job: &Job) -> std::io::Result<Child> {
        std::fs::DirBuilder::new().recursive(true).mode(0o700).create(&job.cache)?;
        let mut command = Command::new(&job.program);
        command
            .args(job.argv(&self.plugins))
            // The worker watches this pipe for EOF, including when the
            // service is replaced or killed. It never uses it for commands.
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped())
            // A group of its own, so what it starts (a shell's programs)
            // ends with it: stop() signals the whole group.
            .process_group(0);
        // Lower than pinholed by NICE, whatever it lowers itself to after.
        // SAFETY: nice(2) is async-signal-safe, and nothing else runs
        // between fork and exec.
        unsafe {
            command.pre_exec(|| {
                libc::nice(NICE);
                Ok(())
            });
        }
        let mut child = command.spawn()?;
        eprintln!("pinholed: {plugin} {name}: started {} (pid {})", job.program.display(), child.id());
        let label = format!("{plugin} {name}");
        if let Some(out) = child.stdout.take() {
            let label = label.clone();
            std::thread::spawn(move || crate::process::log_lines(&label, out, &mut std::io::stderr()));
        }
        if let Some(err) = child.stderr.take() {
            std::thread::spawn(move || crate::process::log_lines(&label, err, &mut std::io::stderr()));
        }
        Ok(child)
    }
}

fn describe(exit: &Exit) -> String {
    match (exit.code, exit.signal) {
        (Some(code), _) => format!("exit code {code}"),
        (_, Some(signal)) => format!("signal {signal}"),
        _ => "lost".to_owned(),
    }
}

/// Close each one's stdin, wait at most five seconds for them all, then
/// kill whatever is left.
fn stop(mut children: Vec<Child>) {
    if children.is_empty() {
        return;
    }
    for child in children.iter_mut() {
        drop(child.stdin.take());
    }
    let deadline = Instant::now() + GRACE;
    while children.iter_mut().any(|c| matches!(c.try_wait(), Ok(None))) && Instant::now() < deadline {
        std::thread::sleep(Duration::from_millis(50));
    }
    for child in children.iter_mut() {
        // Its group, before it is waited for (its id not yet free for
        // another): whatever it started and left running goes too.
        // SAFETY: kill(2) with a negative pid signals that process group.
        unsafe { libc::kill(-(child.id() as libc::pid_t), libc::SIGKILL) };
        let _ = child.wait();
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::os::unix::fs::PermissionsExt;

    struct Scratch {
        dir: PathBuf,
        workers: Workers,
    }

    impl Scratch {
        fn new(name: &str) -> Scratch {
            let dir = std::env::temp_dir().join(format!("pinhole-workers-{name}-{}", std::process::id()));
            let _ = std::fs::remove_dir_all(&dir);
            let places = Places { builtin: dir.join("builtin"), personal: dir.join("personal"), state: dir.join("state") };
            let workers = Workers::new(Some(places), dir.join("plugins.json"), dir.join("cache"));
            Scratch { dir, workers }
        }

        /// A plugin whose worker writes what it is given, then waits for
        /// its stdin to close (or exits with `exit`).
        fn plugin(&self, builtin: bool, folder: &str, id: &str, exit: Option<i32>) {
            let at = self.dir.join(if builtin { "builtin" } else { "personal" }).join(folder);
            std::fs::create_dir_all(&at).unwrap();
            std::fs::write(at.join("Main.qml"), "import Pinhole.Plugin\nPinholePlugin {}\n").unwrap();
            let manifest = json!({"schemaVersion": 1, "api": 11, "id": id, "entryPoints": {"main": "Main.qml"},
                                  "workers": [{"name": "w", "program": "work", "args": ["daemon"]}]});
            std::fs::write(at.join("manifest.json"), manifest.to_string()).unwrap();
            let end = match exit {
                Some(code) => format!("exit {code}"),
                None => "cat > /dev/null\necho bye >&2".to_owned(),
            };
            let log = self.dir.join(format!("{id}.args"));
            std::fs::write(at.join("work"), format!("#!/bin/sh\necho \"$@\" >> '{}'\n{end}\n", log.display())).unwrap();
            std::fs::set_permissions(at.join("work"), std::fs::Permissions::from_mode(0o755)).unwrap();
        }

        fn switches(&self, enabled: Value) {
            std::fs::write(self.dir.join("plugins.json"), json!({"enabled": enabled}).to_string()).unwrap();
        }

        fn args(&self, id: &str) -> Vec<String> {
            std::fs::read_to_string(self.dir.join(format!("{id}.args"))).unwrap_or_default().lines().map(str::to_owned).collect()
        }

        fn shown(&self, name: &str) -> Shown {
            Shown { library: self.dir.join("library").join("folder").join(name), key: format!("folder:/{name}") }
        }

        fn running(&self) -> Vec<(String, bool)> {
            self.workers.status.lock().unwrap().iter().map(|s| (s.plugin.clone(), s.running)).collect()
        }
    }

    impl Drop for Scratch {
        fn drop(&mut self) {
            let _ = std::fs::remove_dir_all(&self.dir);
        }
    }

    /// A round at a time until `done`, five seconds at most.
    fn rounds(s: &Scratch, slots: &mut Vec<Slot>, shown: Option<Shown>, done: impl Fn(&Scratch) -> bool) {
        let deadline = Instant::now() + Duration::from_secs(5);
        loop {
            s.workers.round(slots, shown.clone(), &|_| {});
            if done(s) || Instant::now() > deadline {
                return;
            }
            std::thread::sleep(Duration::from_millis(50));
        }
    }

    #[test]
    fn workers_run_as_their_plugins_switches_say() {
        let s = Scratch::new("switches");
        s.plugin(true, "people", "pinhole.people", None);
        s.plugin(true, "search", "pinhole.search", None);
        s.plugin(false, "mine", "me.mine", None);
        // Your copy of a built-in: off until turned on.
        s.plugin(false, "copy", "pinhole.search", None);
        let shown = s.shown("Pictures");
        let wanted = |s: &Scratch| -> Vec<String> { s.workers.wanted(Some(&shown)).into_iter().map(|(w, _)| w.plugin).collect() };
        assert_eq!(wanted(&s), ["pinhole.people"]);
        // The built-in's switch (turned off and on once) is not the copy's.
        s.switches(json!({"me.mine": true, "pinhole.search": true}));
        assert_eq!(wanted(&s), ["pinhole.people", "me.mine"]);
        s.switches(json!({"me.mine": true, "copy:pinhole.search": true}));
        assert_eq!(wanted(&s), ["pinhole.people", "pinhole.search", "me.mine"]);
        s.switches(json!({"pinhole.people": false, "me.mine": false}));
        assert!(wanted(&s).is_empty());
        // What each is given, its cache the window's for that library.
        s.switches(json!({}));
        let (worker, job) = s.workers.wanted(Some(&shown)).remove(0);
        let job = job.unwrap();
        assert_eq!(worker.program, s.dir.join("builtin/people/work"));
        assert_eq!(job.cache, s.dir.join("cache/pinhole.people/folder-Pictures"));
        let plugins = s.dir.join("plugins.json");
        let argv: Vec<String> = job.argv(&plugins).iter().map(|a| a.to_string_lossy().into_owned()).collect();
        let (library, cache, plugins) = (shown.library.to_string_lossy(), job.cache.to_string_lossy(), plugins.to_string_lossy());
        assert_eq!(argv, ["daemon", "--library", &library, "--library-key", "folder:/Pictures", "--cache-dir", &cache, "--managed-plugins", &plugins]);
        // No source shown: wanted, with nothing to run for.
        assert!(s.workers.wanted(None)[0].1.is_none());
    }

    #[test]
    fn a_worker_runs_for_the_source_shown_and_is_told_of() {
        let s = Scratch::new("run");
        s.plugin(true, "people", "pinhole.people", None);
        let mut slots = Vec::new();
        let events = std::cell::RefCell::new(Vec::new());
        let (a, b) = (s.shown("A"), s.shown("B"));
        s.workers.round(&mut slots, Some(a.clone()), &|e| events.borrow_mut().push(e));
        assert_eq!(s.running(), [("pinhole.people".to_owned(), true)]);
        let event = events.borrow()[0].clone();
        assert_eq!(event["event"], "workers");
        let one = &event["workers"][0];
        assert_eq!((one["plugin"].as_str(), one["name"].as_str(), one["running"].as_bool()), (Some("pinhole.people"), Some("w"), Some(true)));
        assert_eq!(one["key"], "folder:/A");
        assert_eq!(one["last_exit"], Value::Null);
        assert!(one["since"].as_u64().unwrap() > 0);
        assert!(s.dir.join("cache/pinhole.people/folder-A").is_dir());
        rounds(&s, &mut slots, Some(a.clone()), |s| !s.args("pinhole.people").is_empty());
        assert!(s.args("pinhole.people")[0].starts_with("daemon --library "));
        // Nothing changed: nothing told.
        s.workers.round(&mut slots, Some(a.clone()), &|e| events.borrow_mut().push(e));
        assert_eq!(events.borrow().len(), 1);
        // Another source: stopped, and started for that one.
        s.workers.round(&mut slots, Some(b.clone()), &|e| events.borrow_mut().push(e));
        assert_eq!(events.borrow().len(), 2);
        rounds(&s, &mut slots, Some(b.clone()), |s| s.args("pinhole.people").len() == 2);
        assert!(s.args("pinhole.people")[1].contains("folder:/B"), "{:?}", s.args("pinhole.people"));
        assert_eq!(s.workers.report()["workers"][0]["key"], "folder:/B");
        // It ended as its stdin closed, not killed: no exit to tell of.
        assert_eq!(s.workers.report()["workers"][0]["last_exit"], Value::Null);
        // None shown: stopped, still listed.
        s.workers.round(&mut slots, None, &|_| {});
        assert_eq!(s.running(), [("pinhole.people".to_owned(), false)]);
        assert_eq!(s.workers.report()["workers"][0]["key"], Value::Null);
        // Turned off: stopped, and no longer listed.
        s.workers.round(&mut slots, Some(a.clone()), &|_| {});
        assert_eq!(s.running(), [("pinhole.people".to_owned(), true)]);
        s.switches(json!({"pinhole.people": false}));
        s.workers.round(&mut slots, Some(a), &|_| {});
        assert!(s.running().is_empty());
        assert!(slots.is_empty());
    }

    #[test]
    fn a_worker_that_ends_is_said_to_and_started_again_later() {
        let s = Scratch::new("ends");
        s.plugin(false, "mine", "me.mine", Some(3));
        s.switches(json!({"me.mine": true}));
        let mut slots = Vec::new();
        let shown = s.shown("A");
        rounds(&s, &mut slots, Some(shown.clone()), |s| s.running() == [("me.mine".to_owned(), false)]);
        let report = s.workers.report();
        let one = &report["workers"][0];
        assert_eq!((one["running"].as_bool(), one["last_exit"]["code"].as_i64()), (Some(false), Some(3)), "{report:#}");
        // Not again for ten seconds.
        for _ in 0..5 {
            s.workers.round(&mut slots, Some(shown.clone()), &|_| {});
            std::thread::sleep(Duration::from_millis(50));
        }
        assert_eq!(s.args("me.mine").len(), 1);
        // Then again.
        slots[0].retry = Instant::now();
        s.workers.round(&mut slots, Some(shown.clone()), &|_| {});
        rounds(&s, &mut slots, Some(shown), |s| s.args("me.mine").len() == 2);
        assert_eq!(s.args("me.mine").len(), 2);
    }

    #[test]
    fn the_method_answers_for_now_not_as_of_the_last_round() {
        let s = Scratch::new("now");
        s.plugin(false, "mine", "me.mine", None);
        let (a, b) = (s.shown("A"), s.shown("B"));
        let mut slots = Vec::new();
        s.workers.round(&mut slots, Some(a.clone()), &|_| {});
        assert_eq!(s.workers.report_now(Some(&a))["workers"], json!([]));
        // Just turned on: wanted for the source shown, not running yet.
        s.switches(json!({"me.mine": true}));
        let now = s.workers.report_now(Some(&a));
        let one = &now["workers"][0];
        assert_eq!((one["plugin"].as_str(), one["key"].as_str(), one["running"].as_bool()), (Some("me.mine"), Some("folder:/A"), Some(false)), "{now:#}");
        assert_eq!(s.workers.report()["workers"], json!([]));
        s.workers.round(&mut slots, Some(a.clone()), &|_| {});
        assert_eq!(s.workers.report_now(Some(&a))["workers"][0]["running"], true);
        // Another source shown, before a round: for that one, not running.
        let now = s.workers.report_now(Some(&b));
        assert_eq!((now["workers"][0]["key"].as_str(), now["workers"][0]["running"].as_bool()), (Some("folder:/B"), Some(false)));
        // Turned off: none, before a round too.
        s.switches(json!({"me.mine": false}));
        assert_eq!(s.workers.report_now(Some(&a))["workers"], json!([]));
        s.workers.round(&mut slots, Some(a), &|_| {});
    }

    #[test]
    fn a_library_is_named_in_a_cache_as_the_window_names_it() {
        assert_eq!(cache_name(Path::new("/x/library/icloud/me@example.com")), "icloud-me@example.com");
    }
}
