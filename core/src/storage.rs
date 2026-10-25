//! Where the service keeps the files it fetches, and how much of them: the
//! cache's folder and its size limit, kept in Pinhole's settings, changed
//! from the window while the service runs. Lowering the limit frees space
//! at once; a new folder gets the files moved there in the background,
//! still served from the old one until each has arrived.

use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{mpsc, Mutex};
use std::time::{Duration, SystemTime};

use anyhow::{anyhow, Context, Result};
use serde_json::{json, Value};

/// The smallest limit: room for a few hundred full-size photos.
pub const MIN_CAP: u64 = 1 << 30;
/// The limit when none is set.
pub const DEFAULT_CAP: u64 = 10 << 30;

/// The cache's subfolders, one to a kind of file (docs/service.md).
pub const KINDS: [&str; 5] = ["medium", "full", "original", "video", "live"];

/// `~/.config/pinhole/settings.json`.
pub fn settings_path() -> PathBuf {
    std::env::var_os("XDG_CONFIG_HOME")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from(std::env::var_os("HOME").unwrap_or_default()).join(".config"))
        .join("pinhole/settings.json")
}

/// What the settings say: the cache's folder and limit, either missing.
pub fn read_settings(path: &Path) -> (Option<PathBuf>, Option<u64>) {
    let Ok(text) = std::fs::read_to_string(path) else { return (None, None) };
    let Ok(v) = serde_json::from_str::<Value>(&text) else { return (None, None) };
    (v["cache"].as_str().map(PathBuf::from), v["cache_bytes"].as_u64())
}

pub struct Storage {
    settings: PathBuf,
    state: Mutex<State>,
    /// Wakes the eviction thread to free space now, down to this many bytes.
    evict_now: Mutex<mpsc::Sender<u64>>,
    evict_rx: Mutex<Option<mpsc::Receiver<u64>>>,
    pub freeing: AtomicBool,
    moved: AtomicU64,
}

struct State {
    cache: PathBuf,
    cap: u64,
    /// A move under way: the folder the files are leaving, and how much.
    moving_from: Option<PathBuf>,
    moving_total: u64,
}

impl Storage {
    pub fn new(cache: PathBuf, cap: u64, settings: PathBuf) -> Self {
        let (tx, rx) = mpsc::channel();
        Storage {
            settings,
            state: Mutex::new(State { cache, cap: cap.max(MIN_CAP), moving_from: None, moving_total: 0 }),
            evict_now: Mutex::new(tx),
            evict_rx: Mutex::new(Some(rx)),
            freeing: AtomicBool::new(false),
            moved: AtomicU64::new(0),
        }
    }

    pub fn cache(&self) -> PathBuf {
        self.state.lock().unwrap().cache.clone()
    }

    pub fn cap(&self) -> u64 {
        self.state.lock().unwrap().cap
    }

    /// A file of the cache, where it is now: in the cache's folder, or still
    /// in the one a move is taking the files from.
    pub fn find(&self, relative: &Path) -> Option<PathBuf> {
        let state = self.state.lock().unwrap();
        std::iter::once(&state.cache).chain(state.moving_from.as_ref()).map(|dir| dir.join(relative)).find(|p| p.exists())
    }

    /// What the window shows: where, how much and of what, the limit, and
    /// any move or freeing under way.
    pub fn report(&self, library: &Path) -> Value {
        let (cache, cap, moving_from, moving_total) = {
            let s = self.state.lock().unwrap();
            (s.cache.clone(), s.cap, s.moving_from.clone(), s.moving_total)
        };
        let mut kinds = serde_json::Map::new();
        let mut used = 0;
        for kind in KINDS {
            let mut bytes = size_of(&cache.join(kind));
            if let Some(from) = &moving_from {
                bytes += size_of(&from.join(kind));
            }
            used += bytes;
            kinds.insert(kind.into(), json!(bytes));
        }
        json!({
            "cache": cache,
            "cap_bytes": cap,
            "min_cap_bytes": MIN_CAP,
            "used_bytes": used,
            "kinds": kinds,
            "library": library,
            "library_bytes": size_of(library),
            "freeing": self.freeing.load(Ordering::Relaxed),
            "moving": moving_from.map(|from| json!({
                "from": from,
                "done_bytes": self.moved.load(Ordering::Relaxed),
                "total_bytes": moving_total,
            })),
        })
    }

    /// A new limit, kept in the settings. Below what the cache holds, the
    /// files viewed longest ago go now, down to it.
    pub fn set_cap(&self, cap: u64) -> Result<()> {
        let cap = cap.max(MIN_CAP);
        self.state.lock().unwrap().cap = cap;
        self.save()?;
        let _ = self.evict_now.lock().unwrap().send(cap);
        Ok(())
    }

    /// A new folder for the cache, kept in the settings: new files go there
    /// at once, and the cached ones move over in the background.
    pub fn set_cache(self: &std::sync::Arc<Self>, folder: PathBuf) -> Result<()> {
        if !folder.is_absolute() {
            return Err(anyhow!("not an absolute path: {}", folder.display()));
        }
        let from = {
            let state = self.state.lock().unwrap();
            if state.moving_from.is_some() {
                return Err(anyhow!("a move is under way"));
            }
            state.cache.clone()
        };
        if folder == from {
            return Ok(());
        }
        if folder.starts_with(&from) || from.starts_with(&folder) {
            return Err(anyhow!("{} and {} are inside one another", folder.display(), from.display()));
        }
        std::fs::create_dir_all(&folder).with_context(|| format!("creating {}", folder.display()))?;
        let probe = folder.join(".pinhole-write-test");
        std::fs::write(&probe, b"").with_context(|| format!("{} is not writable", folder.display()))?;
        let _ = std::fs::remove_file(&probe);

        let total = KINDS.iter().map(|k| size_of(&from.join(k))).sum();
        {
            let mut state = self.state.lock().unwrap();
            state.cache = folder.clone();
            state.moving_from = Some(from.clone());
            state.moving_total = total;
        }
        self.moved.store(0, Ordering::Relaxed);
        self.save()?;
        let this = self.clone();
        std::thread::spawn(move || {
            this.move_files(&from, &folder);
            this.state.lock().unwrap().moving_from = None;
            eprintln!("pinholed: cache moved from {} to {}", from.display(), folder.display());
        });
        Ok(())
    }

    fn save(&self) -> Result<()> {
        let (cache, cap) = {
            let s = self.state.lock().unwrap();
            (s.cache.clone(), s.cap)
        };
        if let Some(dir) = self.settings.parent() {
            std::fs::create_dir_all(dir)?;
        }
        let part = self.settings.with_extension("json.part");
        std::fs::write(&part, serde_json::to_vec_pretty(&json!({"cache": cache, "cache_bytes": cap}))?)?;
        std::fs::rename(&part, &self.settings)?;
        Ok(())
    }

    /// Each file over in turn: renamed when both folders are on one disk,
    /// else copied beside its new place and renamed into it, then removed
    /// from the old. The old folders go once empty, and the old cache's own
    /// folder with them if nothing else is in it.
    fn move_files(&self, from: &Path, to: &Path) {
        for kind in KINDS {
            let mut files = Vec::new();
            files_in(&from.join(kind), &mut files);
            for (used, size, path) in files {
                let Ok(relative) = path.strip_prefix(from) else { continue };
                let target = to.join(relative);
                if let Some(dir) = target.parent() {
                    let _ = std::fs::create_dir_all(dir);
                }
                if target.exists() || std::fs::rename(&path, &target).is_ok() {
                    let _ = std::fs::remove_file(&path);
                } else {
                    let part = target.with_extension("part");
                    // Keeping when it was last used, which eviction goes by.
                    let copied = std::fs::copy(&path, &part).is_ok()
                        && std::fs::File::options().write(true).open(&part).and_then(|f| f.set_modified(used)).is_ok();
                    if copied && std::fs::rename(&part, &target).is_ok() {
                        let _ = std::fs::remove_file(&path);
                    } else {
                        let _ = std::fs::remove_file(&part);
                        eprintln!("pinholed: could not move {}", path.display());
                    }
                }
                self.moved.fetch_add(size, Ordering::Relaxed);
            }
            remove_empty(&from.join(kind));
        }
        let _ = std::fs::remove_dir(from);
    }

    /// Every two minutes, past the limit, the files used longest ago go
    /// until the cache is back under nine tenths of it; asked to (a lower
    /// limit), at once and down to the limit itself.
    pub fn evict_loop(&self) {
        let Some(rx) = self.evict_rx.lock().unwrap().take() else { return };
        loop {
            let cap = self.cap();
            self.free(cap, cap / 10 * 9);
            if let Ok(asked) = rx.recv_timeout(Duration::from_secs(120)) {
                self.free(asked, asked);
            }
        }
    }

    /// Past `over` bytes, the files used longest ago go until the cache
    /// holds `down_to`.
    fn free(&self, over: u64, down_to: u64) {
        let cache = self.cache();
        let mut files = Vec::new();
        for kind in KINDS {
            files_in(&cache.join(kind), &mut files);
        }
        let mut total: u64 = files.iter().map(|f| f.1).sum();
        if total <= over {
            return;
        }
        self.freeing.store(true, Ordering::Relaxed);
        files.sort();
        for (_, size, path) in files {
            if total <= down_to {
                break;
            }
            if std::fs::remove_file(&path).is_ok() {
                total -= size;
            }
        }
        self.freeing.store(false, Ordering::Relaxed);
    }
}

/// The files under a folder, with when each was last used and its size;
/// not ones still downloading.
pub fn files_in(dir: &Path, out: &mut Vec<(SystemTime, u64, PathBuf)>) {
    let Ok(entries) = std::fs::read_dir(dir) else { return };
    for entry in entries.flatten() {
        let path = entry.path();
        let Ok(meta) = entry.metadata() else { continue };
        if meta.is_dir() {
            files_in(&path, out);
        } else if path.extension().is_none_or(|e| e != "part") {
            out.push((meta.modified().unwrap_or(SystemTime::UNIX_EPOCH), meta.len(), path));
        }
    }
}

fn size_of(dir: &Path) -> u64 {
    let mut files = Vec::new();
    files_in(dir, &mut files);
    files.iter().map(|f| f.1).sum()
}

fn remove_empty(dir: &Path) {
    if let Ok(entries) = std::fs::read_dir(dir) {
        for entry in entries.flatten() {
            if entry.path().is_dir() {
                remove_empty(&entry.path());
            }
        }
    }
    let _ = std::fs::remove_dir(dir);
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::Arc;

    fn temp(name: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("pinhole-storage-{name}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        dir
    }

    fn file(path: &Path, bytes: usize, age_secs: u64) {
        std::fs::create_dir_all(path.parent().unwrap()).unwrap();
        std::fs::write(path, vec![0u8; bytes]).unwrap();
        let f = std::fs::File::options().write(true).open(path).unwrap();
        f.set_modified(SystemTime::now() - Duration::from_secs(age_secs)).unwrap();
    }

    #[test]
    fn a_lower_limit_frees_the_files_used_longest_ago_at_once() {
        let dir = temp("evict");
        let cache = dir.join("cache");
        file(&cache.join("full/aa/old.jpg"), 600, 300);
        file(&cache.join("full/bb/newer.jpg"), 600, 200);
        file(&cache.join("medium/cc/newest.jpg"), 600, 100);
        let storage = Storage::new(cache.clone(), DEFAULT_CAP, dir.join("settings.json"));
        // Below the floor, the floor, kept in the settings.
        storage.set_cap(1).unwrap();
        assert_eq!(storage.cap(), MIN_CAP);
        assert_eq!(read_settings(&dir.join("settings.json")).1, Some(MIN_CAP));
        // What the eviction thread does when asked, at a test's size.
        storage.free(1300, 1300);
        assert!(!cache.join("full/aa/old.jpg").exists());
        assert!(cache.join("full/bb/newer.jpg").exists());
        assert!(cache.join("medium/cc/newest.jpg").exists());
    }

    #[test]
    fn a_new_folder_gets_the_files_moved_and_serves_them_meanwhile() {
        let dir = temp("move");
        let (from, to) = (dir.join("old/pinhole"), dir.join("new"));
        file(&from.join("full/aa/one.jpg"), 100, 0);
        file(&from.join("video/bb/two.mov"), 300, 0);
        std::fs::write(from.join("unrelated.txt"), b"keep").unwrap();
        let storage = Arc::new(Storage::new(from.clone(), DEFAULT_CAP, dir.join("settings.json")));
        assert!(storage.set_cache(from.join("inside")).is_err());
        assert!(storage.set_cache(PathBuf::from("relative")).is_err());
        storage.set_cache(to.clone()).unwrap();
        assert_eq!(storage.cache(), to);
        for _ in 0..100 {
            if storage.state.lock().unwrap().moving_from.is_none() {
                break;
            }
            std::thread::sleep(Duration::from_millis(10));
        }
        assert!(to.join("full/aa/one.jpg").exists());
        assert!(to.join("video/bb/two.mov").exists());
        assert!(!from.join("full").exists());
        // Not Pinhole's: left where it was, and so the old folder too.
        assert!(from.join("unrelated.txt").exists());
        assert_eq!(storage.find(Path::new("full/aa/one.jpg")), Some(to.join("full/aa/one.jpg")));
        assert_eq!(read_settings(&dir.join("settings.json")).0, Some(to));
    }
}
