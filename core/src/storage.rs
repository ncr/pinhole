//! Where Pinhole keeps its files, and how much room they may take: one
//! folder (`~/.local/share/pinhole` unless the window chose another) with
//! each source's library in `library/<kind>/` (the index and the
//! thumbnails, always kept) and the photos fetched from the source in
//! `cache/`, under one limit for the whole. Kept in Pinhole's settings, changed from the window while the
//! service runs: a lower limit frees space at once; a new folder gets
//! everything moved there in the background, the library first, the photos
//! still served from the old folder until each has arrived.

use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{mpsc, Mutex};
use std::time::{Duration, SystemTime};

use anyhow::{anyhow, Context, Result};
use serde_json::{json, Value};

const GB: u64 = 1 << 30;
/// Room for photos the limit always leaves above the library: a few
/// hundred at full size.
pub const MIN_PHOTOS: u64 = GB;
/// The limit when none is set: the library and this much of photos, and
/// never under `DEFAULT_FLOOR`.
pub const DEFAULT_PHOTOS: u64 = 10 * GB;
pub const DEFAULT_FLOOR: u64 = 20 * GB;

/// The cache's subfolders, one to a kind of file (docs/service.md).
pub const KINDS: [&str; 5] = ["medium", "full", "original", "video", "live"];

/// `~/.config/pinhole/settings.json`.
pub fn settings_path() -> PathBuf {
    std::env::var_os("XDG_CONFIG_HOME")
        .map(PathBuf::from)
        .unwrap_or_else(|| home().join(".config"))
        .join("pinhole/settings.json")
}

fn home() -> PathBuf {
    PathBuf::from(std::env::var_os("HOME").unwrap_or_default())
}

/// `~/.local/share/pinhole`.
pub fn default_location() -> PathBuf {
    std::env::var_os("XDG_DATA_HOME").map(PathBuf::from).unwrap_or_else(|| home().join(".local/share")).join("pinhole")
}

/// Where the cache was before it moved in with the library.
pub fn old_cache() -> PathBuf {
    std::env::var_os("XDG_CACHE_HOME").map(PathBuf::from).unwrap_or_else(|| home().join(".cache")).join("pinhole")
}

/// What the settings say, any part missing.
#[derive(Default, Debug, PartialEq)]
pub struct Settings {
    pub location: Option<PathBuf>,
    pub limit: Option<u64>,
    /// Where the photos come from (docs/sources.md); none is iCloud.
    pub source: Option<Value>,
}

pub fn read_settings(path: &Path) -> Settings {
    let Ok(text) = std::fs::read_to_string(path) else { return Settings::default() };
    let Ok(v) = serde_json::from_str::<Value>(&text) else { return Settings::default() };
    Settings {
        location: v["location"].as_str().map(PathBuf::from),
        limit: v["limit_bytes"].as_u64(),
        source: v.get("source").filter(|s| s.is_object()).cloned(),
    }
}

/// A source's library in the folder Pinhole keeps everything in, in
/// `library/` under its kind: `library/icloud`, `library/folder`.
pub fn library_for(location: &Path, source: &dyn crate::source::Source) -> PathBuf {
    location.join("library").join(source.kind())
}

/// Where Pinhole keeps everything now.
pub fn location() -> PathBuf {
    read_settings(&settings_path()).location.unwrap_or_else(default_location)
}

/// Change the settings file, keeping whatever else is in it, written
/// aside and renamed so a reader never sees half of it.
pub fn update_settings(path: &Path, change: impl FnOnce(&mut Value)) -> Result<()> {
    let mut settings = std::fs::read_to_string(path)
        .ok()
        .and_then(|t| serde_json::from_str::<Value>(&t).ok())
        .filter(Value::is_object)
        .unwrap_or_else(|| json!({}));
    change(&mut settings);
    if let Some(dir) = path.parent() {
        std::fs::create_dir_all(dir)?;
    }
    let part = path.with_extension("json.part");
    std::fs::write(&part, serde_json::to_vec_pretty(&settings)?)?;
    std::fs::rename(&part, path)?;
    Ok(())
}

pub fn round_up_to_gb(bytes: u64) -> u64 {
    bytes.div_ceil(GB) * GB
}

pub struct Storage {
    settings: PathBuf,
    state: Mutex<State>,
    /// Wakes the eviction thread to free space now, down to this many bytes
    /// of photos.
    evict_now: Mutex<mpsc::Sender<u64>>,
    evict_rx: Mutex<Option<mpsc::Receiver<u64>>>,
    pub freeing: AtomicBool,
    moved: AtomicU64,
    /// Held while the library is written (a sync round) or moved.
    pub library_lock: Mutex<()>,
    /// Bumped as the library moves: whoever holds it open opens it again.
    pub library_generation: AtomicU64,
    /// Told where the library went once it has moved.
    on_library_moved: Mutex<Option<Box<dyn Fn(&Path) + Send>>>,
}

struct State {
    location: PathBuf,
    library: PathBuf,
    cache: PathBuf,
    /// As set; none, the default.
    limit: Option<u64>,
    /// A move under way, and the folder of photos it is emptying.
    moving: bool,
    moving_from: Option<PathBuf>,
    moving_total: u64,
}

impl Storage {
    pub fn new(location: PathBuf, library: PathBuf, cache: PathBuf, limit: Option<u64>, settings: PathBuf) -> Self {
        let (tx, rx) = mpsc::channel();
        Storage {
            settings,
            state: Mutex::new(State { location, library, cache, limit, moving: false, moving_from: None, moving_total: 0 }),
            evict_now: Mutex::new(tx),
            evict_rx: Mutex::new(Some(rx)),
            freeing: AtomicBool::new(false),
            moved: AtomicU64::new(0),
            library_lock: Mutex::new(()),
            library_generation: AtomicU64::new(0),
            on_library_moved: Mutex::new(None),
        }
    }

    /// Another source's library from now on (the caller holds
    /// `library_lock`): whoever reads the library is told, as after a move.
    pub fn switch_library(&self, library: PathBuf) {
        let _ = std::fs::create_dir_all(&library);
        self.state.lock().unwrap().library = library.clone();
        self.library_generation.fetch_add(1, Ordering::Relaxed);
        if let Some(tell) = self.on_library_moved.lock().unwrap().as_ref() {
            tell(&library);
        }
    }

    pub fn on_library_moved(&self, tell: impl Fn(&Path) + Send + 'static) {
        *self.on_library_moved.lock().unwrap() = Some(Box::new(tell));
    }

    pub fn location(&self) -> PathBuf {
        self.state.lock().unwrap().location.clone()
    }

    pub fn library(&self) -> PathBuf {
        self.state.lock().unwrap().library.clone()
    }

    pub fn cache(&self) -> PathBuf {
        self.state.lock().unwrap().cache.clone()
    }

    /// The smallest limit, the default one and the one in force, for a
    /// library this big: the library always fits, with room for photos.
    pub fn limits(&self, library_bytes: u64) -> (u64, u64, u64) {
        let least = round_up_to_gb(library_bytes + MIN_PHOTOS);
        let default = DEFAULT_FLOOR.max(round_up_to_gb(library_bytes + DEFAULT_PHOTOS));
        let limit = self.state.lock().unwrap().limit.unwrap_or(default).max(least);
        (least, default, limit)
    }

    /// How much of photos the limit leaves beside the library now.
    fn photo_budget(&self) -> u64 {
        let library = size_of(&self.library());
        self.limits(library).2.saturating_sub(library).max(MIN_PHOTOS)
    }

    /// A file of the cache, where it is now: in the cache's folder, or still
    /// in the one a move is taking the files from.
    pub fn find(&self, relative: &Path) -> Option<PathBuf> {
        let state = self.state.lock().unwrap();
        std::iter::once(&state.cache).chain(state.moving_from.as_ref()).map(|dir| dir.join(relative)).find(|p| p.exists())
    }

    /// What the window shows: where, how much and of what, the limits, and
    /// any move or freeing under way.
    pub fn report(&self) -> Value {
        let (location, library, cache, limit_set, moving, moving_from, moving_total) = {
            let s = self.state.lock().unwrap();
            (s.location.clone(), s.library.clone(), s.cache.clone(), s.limit.is_some(), s.moving, s.moving_from.clone(),
             s.moving_total)
        };
        let library_bytes = size_of(&library);
        let mut kinds = serde_json::Map::new();
        kinds.insert("library".into(), json!(library_bytes));
        let mut used = library_bytes;
        for kind in KINDS {
            let mut bytes = size_of(&cache.join(kind));
            if let Some(from) = &moving_from {
                bytes += size_of(&from.join(kind));
            }
            used += bytes;
            kinds.insert(kind.into(), json!(bytes));
        }
        let (least, default, limit) = self.limits(library_bytes);
        json!({
            "location": location,
            "library": library,
            "cache": cache,
            "limit_bytes": limit,
            "min_limit_bytes": least,
            "default_limit_bytes": default,
            "limit_set": limit_set,
            "used_bytes": used,
            "kinds": kinds,
            "freeing": self.freeing.load(Ordering::Relaxed),
            "moving": moving.then(|| json!({
                "done_bytes": self.moved.load(Ordering::Relaxed),
                "total_bytes": moving_total,
            })),
        })
    }

    /// A new limit for everything, kept in the settings; never below what
    /// the library needs. Below what is kept, the photos viewed longest ago
    /// go now, down to it.
    pub fn set_limit(&self, limit: u64) -> Result<()> {
        let least = self.limits(size_of(&self.library())).0;
        self.state.lock().unwrap().limit = Some(limit.max(least));
        self.save()?;
        let _ = self.evict_now.lock().unwrap().send(self.photo_budget());
        Ok(())
    }

    /// A new folder for everything, kept in the settings: new photos go
    /// there at once, the library moves first and the photos after it, in
    /// the background. A library opened from elsewhere (`--library`) stays.
    pub fn set_location(self: &std::sync::Arc<Self>, folder: PathBuf) -> Result<()> {
        if !folder.is_absolute() {
            return Err(anyhow!("not an absolute path: {}", folder.display()));
        }
        let (from, old_library, old_cache) = {
            let state = self.state.lock().unwrap();
            if state.moving {
                return Err(anyhow!("a move is under way"));
            }
            (state.location.clone(), state.library.clone(), state.cache.clone())
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

        // Every source's library, all in `library/`, the one in use among
        // them, unless it was opened from elsewhere.
        let (old_libraries, new_libraries) = (from.join("library"), folder.join("library"));
        let library_moves = old_library.starts_with(&old_libraries);
        let new_library = folder.join(old_library.strip_prefix(&from).unwrap_or(&old_library));
        if new_libraries.exists() {
            return Err(anyhow!("{} already has a library", folder.display()));
        }
        let new_cache = folder.join("cache");
        let total = KINDS.iter().map(|k| size_of(&old_cache.join(k))).sum::<u64>()
            + if library_moves { size_of(&old_libraries) } else { 0 };
        {
            let mut state = self.state.lock().unwrap();
            state.location = folder.clone();
            state.cache = new_cache.clone();
            state.moving = true;
            state.moving_from = Some(old_cache.clone());
            state.moving_total = total;
        }
        self.moved.store(0, Ordering::Relaxed);
        self.save()?;
        let this = self.clone();
        std::thread::spawn(move || {
            if library_moves {
                if let Err(e) = this.move_library(&old_libraries, &new_libraries, &new_library) {
                    eprintln!("pinholed: the libraries stay in {}: {e:#}", old_libraries.display());
                }
            }
            this.move_files(&old_cache, &new_cache);
            // The old folder goes too once nothing else is in it.
            let _ = std::fs::remove_dir(&from);
            let mut state = this.state.lock().unwrap();
            state.moving = false;
            state.moving_from = None;
            eprintln!("pinholed: moved from {} to {}", from.display(), folder.display());
        });
        Ok(())
    }

    /// Photos in a folder the cache used to be in (before it moved in with
    /// the library): moved into the cache in the background.
    pub fn adopt(self: &std::sync::Arc<Self>, old: PathBuf) {
        let cache = self.cache();
        if old == cache || !KINDS.iter().any(|k| old.join(k).is_dir()) {
            return;
        }
        {
            let mut state = self.state.lock().unwrap();
            if state.moving {
                return;
            }
            state.moving = true;
            state.moving_from = Some(old.clone());
            state.moving_total = KINDS.iter().map(|k| size_of(&old.join(k))).sum();
        }
        self.moved.store(0, Ordering::Relaxed);
        let this = self.clone();
        std::thread::spawn(move || {
            this.move_files(&old, &cache);
            let _ = std::fs::remove_dir(&old);
            let mut state = this.state.lock().unwrap();
            state.moving = false;
            state.moving_from = None;
            eprintln!("pinholed: photos moved from {} to {}", old.display(), cache.display());
        });
    }

    fn save(&self) -> Result<()> {
        let (location, limit) = {
            let s = self.state.lock().unwrap();
            (s.location.clone(), s.limit)
        };
        update_settings(&self.settings, |settings| {
            settings["location"] = json!(location);
            match limit {
                Some(limit) => settings["limit_bytes"] = json!(limit),
                None => drop(settings.as_object_mut().and_then(|s| s.remove("limit_bytes"))),
            }
        })
    }

    /// The libraries over, with nothing writing them meanwhile: renamed on
    /// one disk, else copied beside their new place and renamed into it.
    /// Whoever reads the one in use (`library`, there now) is told, and the
    /// old copy goes a little later, once they have let go of it.
    fn move_library(&self, from: &Path, to: &Path, library: &Path) -> Result<()> {
        let _writing = self.library_lock.lock().unwrap();
        let bytes = size_of(from);
        let copied = if std::fs::rename(from, to).is_ok() {
            false
        } else {
            let part = to.with_extension("part");
            let _ = std::fs::remove_dir_all(&part);
            copy_tree(from, &part, &self.moved).with_context(|| format!("copying the libraries to {}", to.display()))?;
            std::fs::rename(&part, to)?;
            true
        };
        if !copied {
            self.moved.fetch_add(bytes, Ordering::Relaxed);
        }
        self.state.lock().unwrap().library = library.to_owned();
        self.library_generation.fetch_add(1, Ordering::Relaxed);
        if let Some(tell) = self.on_library_moved.lock().unwrap().as_ref() {
            tell(library);
        }
        if copied {
            let old = from.to_owned();
            std::thread::spawn(move || {
                std::thread::sleep(Duration::from_secs(30));
                if old.ends_with("library") && old.is_dir() {
                    let _ = std::fs::remove_dir_all(&old);
                }
                // And the folder it was in, if that is empty now.
                if let Some(parent) = old.parent() {
                    let _ = std::fs::remove_dir(parent);
                }
            });
        }
        Ok(())
    }

    /// Each photo over in turn: renamed when both folders are on one disk,
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

    /// Every two minutes, past what the limit leaves for photos, the ones
    /// used longest ago go until they are back under nine tenths of it;
    /// asked to (a lower limit), at once and down to it.
    pub fn evict_loop(&self) {
        let Some(rx) = self.evict_rx.lock().unwrap().take() else { return };
        loop {
            let budget = self.photo_budget();
            self.free(budget, budget / 10 * 9);
            if let Ok(asked) = rx.recv_timeout(Duration::from_secs(120)) {
                self.free(asked, asked);
            }
        }
    }

    /// Past `over` bytes of photos, the ones used longest ago go until
    /// `down_to` are left.
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

/// A folder and everything in it, counting the bytes as they go.
fn copy_tree(from: &Path, to: &Path, counted: &AtomicU64) -> Result<()> {
    std::fs::create_dir_all(to)?;
    for entry in std::fs::read_dir(from)? {
        let entry = entry?;
        let target = to.join(entry.file_name());
        if entry.file_type()?.is_dir() {
            copy_tree(&entry.path(), &target, counted)?;
        } else {
            counted.fetch_add(std::fs::copy(entry.path(), &target)?, Ordering::Relaxed);
        }
    }
    Ok(())
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

    fn storage_in(location: &Path, settings: &Path) -> Arc<Storage> {
        Arc::new(Storage::new(
            location.to_owned(),
            location.join("library/icloud"),
            location.join("cache"),
            None,
            settings.to_owned(),
        ))
    }

    fn until_moved(storage: &Storage) {
        for _ in 0..200 {
            if !storage.state.lock().unwrap().moving {
                return;
            }
            std::thread::sleep(Duration::from_millis(10));
        }
        panic!("still moving");
    }

    #[test]
    fn the_limit_always_leaves_the_library_room_and_defaults_sensibly() {
        let dir = temp("limits");
        let storage = storage_in(&dir.join("pinhole"), &dir.join("settings.json"));
        // A 3.1 GB library: at least 5 GB, 20 GB unless set.
        let library = 3 * GB + GB / 10;
        assert_eq!(storage.limits(library), (5 * GB, 20 * GB, 20 * GB));
        // A big one: its size and 10 GB, rounded up.
        assert_eq!(storage.limits(15 * GB + 1).1, 26 * GB);
        // Set below the least, the least.
        storage.state.lock().unwrap().limit = Some(2 * GB);
        assert_eq!(storage.limits(library).2, 5 * GB);
    }

    #[test]
    fn a_lower_limit_frees_the_photos_used_longest_ago_at_once() {
        let dir = temp("evict");
        let location = dir.join("pinhole");
        let cache = location.join("cache");
        file(&cache.join("full/aa/old.jpg"), 600, 300);
        file(&cache.join("full/bb/newer.jpg"), 600, 200);
        file(&cache.join("medium/cc/newest.jpg"), 600, 100);
        let storage = storage_in(&location, &dir.join("settings.json"));
        storage.set_limit(1).unwrap();
        // Kept at the least there can be, in the settings with the folder.
        assert_eq!(read_settings(&dir.join("settings.json")), Settings { location: Some(location), limit: Some(GB), source: None });
        // What the eviction thread does when asked, at a test's size.
        storage.free(1300, 1300);
        assert!(!cache.join("full/aa/old.jpg").exists());
        assert!(cache.join("full/bb/newer.jpg").exists());
        assert!(cache.join("medium/cc/newest.jpg").exists());
    }

    #[test]
    fn a_new_folder_gets_the_library_and_the_photos_moved() {
        let dir = temp("move");
        let (from, to) = (dir.join("old/pinhole"), dir.join("new"));
        file(&from.join("library/icloud/library.sqlite"), 50, 0);
        file(&from.join("library/icloud/thumbs/aa/one.jpg"), 20, 0);
        file(&from.join("cache/full/aa/one.jpg"), 100, 0);
        file(&from.join("cache/video/bb/two.mov"), 300, 0);
        let storage = storage_in(&from, &dir.join("settings.json"));
        let told = Arc::new(Mutex::new(None));
        {
            let told = told.clone();
            storage.on_library_moved(move |at| *told.lock().unwrap() = Some(at.to_owned()));
        }
        assert!(storage.set_location(from.join("inside")).is_err());
        assert!(storage.set_location(PathBuf::from("relative")).is_err());
        storage.set_location(to.clone()).unwrap();
        until_moved(&storage);
        assert!(to.join("library/icloud/library.sqlite").exists());
        assert!(to.join("library/icloud/thumbs/aa/one.jpg").exists());
        assert!(to.join("cache/full/aa/one.jpg").exists());
        assert!(to.join("cache/video/bb/two.mov").exists());
        assert!(!from.exists());
        assert_eq!(storage.library(), to.join("library/icloud"));
        assert_eq!(*told.lock().unwrap(), Some(to.join("library/icloud")));
        assert_eq!(storage.library_generation.load(Ordering::Relaxed), 1);
        assert_eq!(read_settings(&dir.join("settings.json")).location, Some(to));
    }

    #[test]
    fn a_move_takes_every_source_library_and_keeps_the_rest_of_the_settings() {
        let dir = temp("move-sources");
        let (from, to) = (dir.join("old"), dir.join("new"));
        file(&from.join("library/folder/library.sqlite"), 50, 0);
        file(&from.join("library/icloud/library.sqlite"), 70, 0);
        file(&from.join("library/icloud/thumbs/aa/one.jpg"), 20, 0);
        let settings = dir.join("settings.json");
        std::fs::write(&settings, r#"{"source": {"kind": "folder", "path": "/photos"}}"#).unwrap();
        // The folder's library in use, iCloud's beside it.
        let storage = Arc::new(Storage::new(from.clone(), from.join("library/folder"), from.join("cache"), None, settings.clone()));
        storage.set_location(to.clone()).unwrap();
        until_moved(&storage);
        assert_eq!(storage.library(), to.join("library/folder"));
        assert!(to.join("library/folder/library.sqlite").exists());
        assert!(to.join("library/icloud/thumbs/aa/one.jpg").exists());
        assert!(!from.exists());
        let saved = read_settings(&settings);
        assert_eq!(saved.location, Some(to));
        assert_eq!(saved.source, Some(json!({"kind": "folder", "path": "/photos"})));
    }

    #[test]
    fn photos_from_where_the_cache_used_to_be_move_in() {
        let dir = temp("adopt");
        let old = dir.join("dot-cache/pinhole");
        file(&old.join("medium/aa/one.jpg"), 100, 0);
        let location = dir.join("pinhole");
        let storage = storage_in(&location, &dir.join("settings.json"));
        storage.adopt(old.clone());
        until_moved(&storage);
        assert!(location.join("cache/medium/aa/one.jpg").exists());
        assert!(!old.exists());
    }
}
