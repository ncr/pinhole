//! Owns the optional local search worker for the shared library.

use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::time::{Duration, Instant};

const PLUGIN: &str = "me.semantic-search";

pub fn helper(plugins: &Path) -> Option<PathBuf> {
    if crate::plugins::switches_in(plugins).get(PLUGIN) != Some(&true) {
        return None;
    }
    let helper = plugins.parent()?.join("plugins").join(PLUGIN).join("pinhole-semantic");
    helper.is_file().then_some(helper)
}

#[derive(PartialEq, Eq)]
pub struct Job {
    pub helper: PathBuf,
    pub library: PathBuf,
    pub key: String,
}

impl Job {
    fn spawn(&self, plugins: &Path) -> std::io::Result<Child> {
        Command::new(&self.helper)
            .arg("daemon")
            .arg("--library").arg(&self.library)
            .arg("--library-key").arg(&self.key)
            .arg("--managed-plugins").arg(plugins)
            // The worker watches this pipe for EOF, including when the
            // daemon is replaced or killed. It never uses it for commands.
            .stdin(Stdio::piped())
            .spawn()
    }
}

pub fn supervise(plugins: &Path, current: impl Fn() -> Option<Job>) {
    let mut child: Option<(Job, Child)> = None;
    let mut retry = Instant::now();
    loop {
        let wanted = current();
        if let Some((job, process)) = child.as_mut() {
            if wanted.as_ref() != Some(job) {
                stop(process);
                child = None;
                retry = Instant::now();
            } else if !matches!(process.try_wait(), Ok(None)) {
                child = None;
                retry = Instant::now() + Duration::from_secs(10);
            }
        }
        if child.is_none() && Instant::now() >= retry {
            if let Some(job) = wanted {
                match job.spawn(plugins) {
                    Ok(process) => child = Some((job, process)),
                    Err(error) => {
                        eprintln!("pinholed: could not start local photo search: {error}");
                        retry = Instant::now() + Duration::from_secs(10);
                    }
                }
            }
        }
        std::thread::sleep(Duration::from_secs(2));
    }
}

fn stop(child: &mut Child) {
    drop(child.stdin.take());
    let deadline = Instant::now() + Duration::from_secs(5);
    while matches!(child.try_wait(), Ok(None)) && Instant::now() < deadline {
        std::thread::sleep(Duration::from_millis(50));
    }
    if matches!(child.try_wait(), Ok(None)) {
        let _ = child.kill();
        let _ = child.wait();
    }
}
