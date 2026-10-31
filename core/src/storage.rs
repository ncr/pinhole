//! Where Pinhole keeps its files, and how much room they may take: one
//! folder (`~/.local/share/pinhole` unless the window chose another) with
//! each source's library in `library/<kind>/<name>/` (the index and the
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
    /// Every source added, in order (docs/sources.md).
    pub sources: Vec<Value>,
    /// The one shown, if any.
    pub source: Option<Value>,
}

pub fn read_settings(path: &Path) -> Settings {
    let written = std::fs::read_to_string(path)
        .ok()
        .and_then(|t| serde_json::from_str::<Value>(&t).ok())
        .filter(Value::is_object)
        .unwrap_or_else(|| json!({}));
    resolve(&written, &crate::icloud::auth::accounts())
}

/// The settings as written, with the Apple IDs signed in to on this
/// computer: without `sources`, those Apple IDs then the folder shown if
/// it is one; without `source`, the first of them (`null` is none).
fn resolve(v: &Value, accounts: &[String]) -> Settings {
    // An iCloud entry naming no Apple ID is the only one signed in to.
    let normal = |entry: &Value| -> Option<Value> {
        match entry["kind"].as_str()? {
            "icloud" => {
                let account = entry["account"].as_str().map(str::to_owned).or_else(|| (accounts.len() == 1).then(|| accounts[0].clone()))?;
                Some(json!({"kind": "icloud", "account": account}))
            }
            "folder" => Some(json!({"kind": "folder", "path": entry["path"].as_str()?})),
            _ => None,
        }
    };
    let shown = match v.get("source") {
        None => None,
        Some(Value::Null) => Some(None),
        Some(entry) => Some(normal(entry)),
    };
    let mut sources: Vec<Value> = match v["sources"].as_array() {
        Some(list) => list.iter().filter_map(normal).collect(),
        None => {
            let mut list: Vec<Value> = accounts.iter().map(|a| json!({"kind": "icloud", "account": a})).collect();
            if let Some(Some(entry)) = &shown {
                if entry["kind"] == "folder" {
                    list.push(entry.clone());
                }
            }
            list
        }
    };
    let mut seen = std::collections::HashSet::new();
    sources.retain(|e| seen.insert(source_id(e)));
    let source = match shown {
        Some(entry) => entry,
        None => sources.first().cloned(),
    };
    Settings {
        location: v["location"].as_str().map(PathBuf::from),
        limit: v["limit_bytes"].as_u64(),
        sources,
        source,
    }
}

/// A settings entry's id, as its source's `id()` says it: `icloud:<apple
/// id>`, `folder:<path>`.
pub fn source_id(entry: &Value) -> String {
    match entry["kind"].as_str() {
        Some("icloud") => format!("icloud:{}", entry["account"].as_str().unwrap_or("")),
        Some("folder") => format!("folder:{}", entry["path"].as_str().unwrap_or("")),
        _ => "none".into(),
    }
}

/// The entry an id names, among those added.
pub fn source_entry(settings: &Settings, id: &str) -> Option<Value> {
    settings.sources.iter().find(|e| source_id(e) == id).cloned()
}

/// Show `entry`, adding it to the sources first if it is new; `None`
/// shows nothing.
pub fn show_source(path: &Path, entry: Option<&Value>) -> Result<()> {
    let _writing = SETTINGS.lock().unwrap_or_else(|e| e.into_inner());
    let current = read_settings(path);
    write_settings(path, |settings| {
        let mut sources = current.sources.clone();
        if let Some(entry) = entry {
            if !sources.iter().any(|e| source_id(e) == source_id(entry)) {
                sources.push(entry.clone());
            }
        }
        settings["sources"] = json!(sources);
        settings["source"] = entry.cloned().unwrap_or(Value::Null);
    })
}

/// Take a source out of the list; the one shown, if it was that, becomes
/// the first left (or none). The entry shown afterwards.
pub fn remove_source(path: &Path, id: &str) -> Result<Option<Value>> {
    let _writing = SETTINGS.lock().unwrap_or_else(|e| e.into_inner());
    let current = read_settings(path);
    let sources: Vec<Value> = current.sources.iter().filter(|e| source_id(e) != id).cloned().collect();
    let shown = match &current.source {
        Some(entry) if source_id(entry) != id => Some(entry.clone()),
        _ => sources.first().cloned(),
    };
    write_settings(path, |settings| {
        settings["sources"] = json!(sources);
        settings["source"] = shown.clone().unwrap_or(Value::Null);
    })?;
    Ok(shown)
}

/// A source's library in the folder Pinhole keeps everything in, one to a
/// source, under its kind: `library/icloud/<apple-id>`,
/// `library/folder/home-me-Pictures`; with no source, an empty one in
/// `library/none`. Never another source's (by what each says it is,
/// `library_owner`), and always the same one: its own wherever it is (its
/// name, a former one, or one by its id it was given when its name was
/// another's), else the first of those that is no one's.
pub fn library_for(location: &Path, source: &dyn crate::source::Source) -> PathBuf {
    if source.kind() == "none" {
        return location.join("library/none");
    }
    let kind = location.join("library").join(source.kind());
    let id = source.id();
    let name = source.library_name();
    // By its id: the first, and more should even that be another's.
    let by_id = |n: usize| {
        let hash = format!("{name}~{:016x}", stable_hash(id.as_bytes()));
        kind.join(if n == 0 { hash } else { format!("{hash}-{n}") })
    };
    let by_ids = (0..).map(by_id).take_while(|dir| dir.exists());
    let formers = source.former_library_names().into_iter().map(|former| kind.join(former));
    if let Some(own) = std::iter::once(kind.join(&name))
        .chain(formers)
        .chain(by_ids)
        .find(|dir| library_owner(dir).as_deref() == Some(id.as_str()))
    {
        return own;
    }
    std::iter::once(kind.join(&name))
        .chain((0..).map(by_id))
        .find(|dir| library_owner(dir).is_none())
        .expect("a name no library has")
}

/// Which source a library holds the photos of (`sync::claim`); None for
/// none yet, or no library there.
pub fn library_owner(library: &Path) -> Option<String> {
    let db = rusqlite::Connection::open_with_flags(library.join("library.sqlite"), rusqlite::OpenFlags::SQLITE_OPEN_READ_ONLY).ok()?;
    crate::db::meta(&db, "source")
}

/// FNV-1a: the same in every build, unlike std's hasher, for names on disk.
pub fn stable_hash(bytes: &[u8]) -> u64 {
    bytes.iter().fold(0xcbf29ce484222325, |hash, &b| (hash ^ b as u64).wrapping_mul(0x100000001b3))
}

/// Where Pinhole keeps everything now.
pub fn location() -> PathBuf {
    read_settings(&settings_path()).location.unwrap_or_else(default_location)
}

/// Change the settings file, keeping whatever else is in it, written
/// aside and renamed so a reader never sees half of it.
pub fn update_settings(path: &Path, change: impl FnOnce(&mut Value)) -> Result<()> {
    let _writing = SETTINGS.lock().unwrap_or_else(|e| e.into_inner());
    write_settings(path, change)
}

/// One change of the settings at a time in this process: each reads what
/// the one before wrote.
static SETTINGS: Mutex<()> = Mutex::new(());

/// For a test: run once in `save`, between reading what is in force and
/// writing it, as another thread could come between them.
#[cfg(test)]
#[allow(clippy::type_complexity)]
static SAVE_BETWEEN: Mutex<Option<Box<dyn FnOnce() + Send>>> = Mutex::new(None);

fn write_settings(path: &Path, change: impl FnOnce(&mut Value)) -> Result<()> {
    let mut settings = std::fs::read_to_string(path)
        .ok()
        .and_then(|t| serde_json::from_str::<Value>(&t).ok())
        .filter(Value::is_object)
        .unwrap_or_else(|| json!({}));
    change(&mut settings);
    if let Some(dir) = path.parent() {
        std::fs::create_dir_all(dir)?;
    }
    // Named for this process: pinhole-sync may be writing them too.
    let part = path.with_extension(format!("json.{}.part", std::process::id()));
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
    /// Written for a moment (a favourite, fresh links) under its read side;
    /// copied elsewhere under its write side, which waits for those.
    pub library_moving: std::sync::RwLock<()>,
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
    /// While the libraries go over, the folder they are going to: the
    /// window is told that one, as it is where everything will be.
    moving_to: Option<PathBuf>,
}

impl Storage {
    pub fn new(location: PathBuf, library: PathBuf, cache: PathBuf, limit: Option<u64>, settings: PathBuf) -> Self {
        let (tx, rx) = mpsc::channel();
        Storage {
            settings,
            state: Mutex::new(State { location, library, cache, limit, moving: false, moving_from: None, moving_total: 0, moving_to: None }),
            evict_now: Mutex::new(tx),
            evict_rx: Mutex::new(Some(rx)),
            freeing: AtomicBool::new(false),
            moved: AtomicU64::new(0),
            library_lock: Mutex::new(()),
            library_generation: AtomicU64::new(0),
            library_moving: std::sync::RwLock::new(()),
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
            (s.moving_to.clone().unwrap_or_else(|| s.location.clone()), s.library.clone(), s.cache.clone(), s.limit.is_some(),
             s.moving, s.moving_from.clone(), s.moving_total)
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
        if new_libraries.exists() {
            return Err(anyhow!("{} already has a library", folder.display()));
        }
        let total = KINDS.iter().map(|k| size_of(&old_cache.join(k))).sum::<u64>()
            + if library_moves { size_of(&old_libraries) } else { 0 };
        {
            let mut state = self.state.lock().unwrap();
            state.moving = true;
            state.moving_from = None;
            state.moving_total = total;
            state.moving_to = Some(folder.clone());
        }
        self.moved.store(0, Ordering::Relaxed);
        // The plan first, so a move cut short (the service stopped, the
        // computer off) is put right as the service starts again
        // (`resume`): until the libraries are over, the settings still say
        // the old folder, which keeps everything.
        if let Err(e) = self.plan(Some((&from, &folder, &old_cache, "library"))) {
            let mut state = self.state.lock().unwrap();
            state.moving = false;
            state.moving_to = None;
            return Err(e);
        }
        let this = self.clone();
        std::thread::spawn(move || {
            let commit = || this.commit(&from, &folder, &old_cache);
            let moved = if library_moves { this.move_library(&old_libraries, &new_libraries, commit) } else { commit() };
            if let Err(e) = moved {
                // All of it stays where it was, the settings too.
                eprintln!("pinholed: the move to {} is given up; everything stays in {}: {e:#}", folder.display(), from.display());
                let _ = std::fs::remove_dir_all(new_libraries.with_extension("part"));
                let _ = this.plan(None);
                let mut state = this.state.lock().unwrap();
                state.moving = false;
                state.moving_to = None;
                return;
            }
            this.photos_over(&from, &folder, &old_cache);
        });
        Ok(())
    }

    /// The point a move cannot go back from: the new folder in the
    /// settings, with the plan's photos half, in one write, and in force
    /// (new photos go there at once). Made while nothing writes the
    /// libraries (move_library), so none is written in the new folder
    /// before the settings say it; a failure to write it gives the move up.
    fn commit(&self, from: &Path, to: &Path, old_cache: &Path) -> Result<()> {
        // Written and in force under the settings' lock: whoever writes
        // them next (a new limit) writes this folder.
        let _settings = SETTINGS.lock().unwrap_or_else(|e| e.into_inner());
        write_settings(&self.settings, |settings| {
            settings["location"] = json!(to);
            settings["moving"] = json!({"from": from, "to": to, "cache": old_cache, "phase": "photos"});
        })?;
        let mut state = self.state.lock().unwrap();
        state.location = to.to_owned();
        state.cache = to.join("cache");
        state.moving_to = None;
        state.moving = true;
        state.moving_from = Some(old_cache.to_owned());
        Ok(())
    }

    /// The second half of a move, once committed: the photos kept moved
    /// over in the background (each served from the old folder until it
    /// has moved). Then the old folder goes, and the plan.
    fn photos_over(&self, from: &Path, to: &Path, old_cache: &Path) {
        let (old_cache, new_cache) = (old_cache.to_owned(), to.join("cache"));
        self.move_files(&old_cache, &new_cache);
        // The old folder goes too once nothing else is in it.
        let _ = std::fs::remove_dir(from);
        let _ = self.plan(None);
        let mut state = self.state.lock().unwrap();
        state.moving = false;
        state.moving_from = None;
        eprintln!("pinholed: moved from {} to {}", from.display(), to.display());
    }

    /// A move under way, kept in the settings as `moving`: from, to, the
    /// photos' old folder, and which half (`library` or `photos`); None,
    /// done.
    fn plan(&self, plan: Option<(&Path, &Path, &Path, &str)>) -> Result<()> {
        update_settings(&self.settings, |settings| match plan {
            Some((from, to, cache, phase)) => {
                settings["moving"] = json!({"from": from, "to": to, "cache": cache, "phase": phase})
            }
            None => drop(settings.as_object_mut().and_then(|s| s.remove("moving"))),
        })
    }

    /// A move cut short, put right as the service starts, before anything
    /// reads the library. Committed (the photos half), the photos are moved
    /// on. Not, it is undone and everything stays in the old folder: a copy
    /// made in the new one was never written, as nothing writes before the
    /// commit. Only libraries renamed over (one step) with no old ones left
    /// are committed now and gone on with.
    pub fn resume(self: &std::sync::Arc<Self>) -> Result<()> {
        let written = std::fs::read_to_string(&self.settings)
            .ok()
            .and_then(|t| serde_json::from_str::<Value>(&t).ok())
            .unwrap_or(Value::Null);
        let plan = &written["moving"];
        let (Some(from), Some(to)) = (plan["from"].as_str().map(PathBuf::from), plan["to"].as_str().map(PathBuf::from)) else {
            return Ok(());
        };
        let old_cache = plan["cache"].as_str().map_or_else(|| from.join("cache"), PathBuf::from);
        let (old_libraries, new_libraries) = (from.join("library"), to.join("library"));
        let committed = plan["phase"] == "photos";
        let renamed = !old_libraries.exists() && new_libraries.exists();
        if !committed && !renamed {
            eprintln!("pinholed: a move to {} was cut short; everything stays in {}", to.display(), from.display());
            let _ = std::fs::remove_dir_all(new_libraries.with_extension("part"));
            // A whole copy published but not committed: never written.
            if old_libraries.exists() {
                let _ = std::fs::remove_dir_all(&new_libraries);
            }
            let _ = std::fs::remove_dir(&to);
            let _ = self.plan(None);
            return Ok(());
        }
        if !committed {
            if let Err(e) = self.commit(&from, &to, &old_cache) {
                // Undone instead: the libraries back where the settings
                // say, the plan's libraries half then undone at the next
                // start. Should even that fail, nothing is touched and the
                // service does not start: the next start commits them.
                eprintln!("pinholed: going on with the move to {}: {e:#}", to.display());
                let back = std::fs::create_dir_all(&from).and_then(|()| std::fs::rename(&new_libraries, &old_libraries));
                if let Err(back) = back {
                    return Err(anyhow!(
                        "the libraries are in {} but the settings cannot say so ({e:#}), nor can they go back ({back}); \
                         make the settings writable and start again",
                        to.display()
                    ));
                }
                let _ = self.plan(None);
                return Ok(());
            }
        }
        eprintln!("pinholed: going on with the move from {} to {}", from.display(), to.display());
        {
            let mut state = self.state.lock().unwrap();
            // The library in use, if it was among them: its place there.
            if let Ok(inside) = state.library.strip_prefix(&from) {
                state.library = to.join(inside);
            }
            state.location = to.clone();
            state.cache = to.join("cache");
            state.moving = true;
            state.moving_from = Some(old_cache.clone());
            state.moving_total = KINDS.iter().map(|k| size_of(&old_cache.join(k))).sum();
        }
        // The old libraries, if a copy of them was left behind.
        if old_libraries.exists() && new_libraries.exists() {
            let _ = std::fs::remove_dir_all(&old_libraries);
        }
        self.moved.store(0, Ordering::Relaxed);
        let this = self.clone();
        std::thread::spawn(move || this.photos_over(&from, &to, &old_cache));
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
        // What is in force read under the settings' lock, which a move's
        // commit holds until it is in force: never a folder a move has
        // just left.
        let _settings = SETTINGS.lock().unwrap_or_else(|e| e.into_inner());
        let (location, limit) = {
            let s = self.state.lock().unwrap();
            (s.location.clone(), s.limit)
        };
        #[cfg(test)]
        if let Some(between) = SAVE_BETWEEN.lock().unwrap().take() {
            between();
        }
        write_settings(&self.settings, |settings| {
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
    fn move_library(&self, from: &Path, to: &Path, commit: impl FnOnce() -> Result<()>) -> Result<()> {
        // Nothing writes them meanwhile: not a round (library_lock), not a
        // favourite or a lookup (library_moving), not pinhole-sync (each
        // library's writer).
        let _writing = self.library_lock.lock().unwrap();
        let _moving = self.library_moving.write().unwrap();
        let mut writers = Vec::new();
        for kind in std::fs::read_dir(from).into_iter().flatten().flatten() {
            for one in std::fs::read_dir(kind.path()).into_iter().flatten().flatten() {
                if one.path().join("library.sqlite").exists() {
                    writers.push(crate::sync::Writer::take(&one.path())?);
                }
            }
        }
        // The library in use now, under these locks (another source's may
        // have been shown since the move was asked for): its place there.
        let library = {
            let current = self.state.lock().unwrap().library.clone();
            current.strip_prefix(from).map(|inside| to.join(inside)).ok()
        };
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
        // Over: the settings say so before anything writes them there.
        // If they cannot, it is undone, as if it had not begun.
        if let Err(e) = commit() {
            if copied {
                let _ = std::fs::remove_dir_all(to);
            } else {
                let _ = std::fs::rename(to, from);
            }
            return Err(e);
        }
        if !copied {
            self.moved.fetch_add(bytes, Ordering::Relaxed);
        }
        if let Some(library) = &library {
            self.state.lock().unwrap().library = library.clone();
            self.library_generation.fetch_add(1, Ordering::Relaxed);
            if let Some(tell) = self.on_library_moved.lock().unwrap().as_ref() {
                tell(library);
            }
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

    #[test]
    fn folders_whose_paths_differ_have_libraries_apart() {
        use crate::folder::FolderSource;
        let location = temp("names");
        let for_path = |p: &str| library_for(&location, &FolderSource::new(PathBuf::from(p)));
        assert_ne!(for_path("/a-b/c"), for_path("/a/b-c"));
        assert_ne!(for_path("/root"), for_path("/"));
        // A tilde of its own is told apart from the hash's.
        assert_ne!(for_path("/home/me/My-Photos"), for_path("/home/me/My/Photos~7f094097f3a39b1b"));
        assert_eq!(for_path("/home/me/Pictures"), location.join("library/folder/home-me-Pictures"));
        // The same as the window's (Storage::library) test of this path.
        assert_eq!(for_path("/home/me/My-Photos"), location.join("library/folder/home-me-My-Photos~7f094097f3a39b1b"));

        // One made under the name a path with a dash had before is kept;
        // another folder whose name that is now has one by its id, and one
        // that only had it before has its new one.
        let own = location.join("library/folder/a-b-c");
        std::fs::create_dir_all(&own).unwrap();
        let db = crate::db::open(&own.join("library.sqlite")).unwrap();
        crate::db::set_meta(&db, "source", "folder:/a/b-c").unwrap();
        drop(db);
        assert_eq!(for_path("/a/b-c"), own);
        assert_eq!(for_path("/a/b/c"), location.join("library/folder/a-b-c~43215282df502c0e"));
        assert_ne!(for_path("/a-b/c"), own);
        // One with no owner yet is anyone's to claim.
        assert_eq!(for_path("/x/y"), location.join("library/folder/x-y"));
        // That name by its id another's too (as a path with a tilde and no
        // dash had it before): the next.
        let claimed = |dir: &Path, owner: &str| {
            std::fs::create_dir_all(dir).unwrap();
            crate::db::set_meta(&crate::db::open(&dir.join("library.sqlite")).unwrap(), "source", owner).unwrap();
        };
        let by_id = location.join("library/folder/a-b-c~43215282df502c0e");
        claimed(&by_id, "folder:/a/b/c~43215282df502c0e");
        let next = location.join("library/folder/a-b-c~43215282df502c0e-1");
        assert_eq!(for_path("/a/b/c"), next);
        // Its own there, it is kept to, even once its name is no one's.
        claimed(&next, "folder:/a/b/c");
        assert_eq!(for_path("/a/b/c"), next);
        std::fs::remove_dir_all(&own).unwrap();
        assert_eq!(for_path("/a/b/c"), next);
        let _ = std::fs::remove_dir_all(&location);
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
        let saved = read_settings(&dir.join("settings.json"));
        assert_eq!((saved.location, saved.limit), (Some(location), Some(GB)));
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
        // And no plan left.
        let written: Value = serde_json::from_str(&std::fs::read_to_string(dir.join("settings.json")).unwrap()).unwrap();
        assert!(written.get("moving").is_none());
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
    fn a_move_cut_short_before_the_libraries_were_over_leaves_everything_where_it_was() {
        let dir = temp("cut-library");
        let (from, to) = (dir.join("old"), dir.join("new"));
        file(&from.join("library/icloud/library.sqlite"), 50, 0);
        file(&from.join("cache/full/aa/one.jpg"), 100, 0);
        // Half a copy beside the new place, and the plan, as a service
        // stopped while copying across disks leaves them.
        file(&to.join("library.part/icloud/library.sqlite"), 20, 0);
        let settings = dir.join("settings.json");
        std::fs::write(&settings, json!({"location": from, "moving": {"from": from, "to": to,
            "cache": from.join("cache"), "phase": "library"}}).to_string()).unwrap();
        let storage = storage_in(&from, &settings);
        storage.resume().unwrap();
        assert!(!storage.state.lock().unwrap().moving);
        assert_eq!(storage.library(), from.join("library/icloud"));
        assert!(from.join("library/icloud/library.sqlite").exists());
        assert!(from.join("cache/full/aa/one.jpg").exists());
        assert!(!to.exists());
        let written: Value = serde_json::from_str(&std::fs::read_to_string(&settings).unwrap()).unwrap();
        assert!(written.get("moving").is_none());
        assert_eq!(read_settings(&settings).location, Some(from));
    }

    #[test]
    fn a_move_cut_short_once_the_libraries_were_over_goes_on_with_the_photos() {
        let dir = temp("cut-photos");
        let (from, to) = (dir.join("old"), dir.join("new"));
        // The libraries renamed over, the settings not told yet.
        file(&to.join("library/icloud/library.sqlite"), 50, 0);
        file(&from.join("cache/full/aa/one.jpg"), 100, 0);
        let settings = dir.join("settings.json");
        std::fs::write(&settings, json!({"location": from, "moving": {"from": from, "to": to,
            "cache": from.join("cache"), "phase": "library"}}).to_string()).unwrap();
        let storage = storage_in(&from, &settings);
        storage.resume().unwrap();
        // The library is read from its new place at once.
        assert_eq!(storage.library(), to.join("library/icloud"));
        until_moved(&storage);
        assert!(to.join("cache/full/aa/one.jpg").exists());
        assert!(!from.exists());
        assert_eq!(storage.cache(), to.join("cache"));
        let written: Value = serde_json::from_str(&std::fs::read_to_string(&settings).unwrap()).unwrap();
        assert!(written.get("moving").is_none());
        assert_eq!(read_settings(&settings).location, Some(to));
    }

    #[test]
    fn a_whole_copy_never_committed_is_not_taken_for_the_library() {
        let dir = temp("cut-copy");
        let (from, to) = (dir.join("old"), dir.join("new"));
        // Copied across disks and renamed into place, the service stopped
        // before the commit: both there, the plan's libraries half.
        file(&from.join("library/icloud/library.sqlite"), 50, 0);
        file(&to.join("library/icloud/library.sqlite"), 50, 0);
        let settings = dir.join("settings.json");
        std::fs::write(&settings, json!({"location": from, "moving": {"from": from, "to": to,
            "cache": from.join("cache"), "phase": "library"}}).to_string()).unwrap();
        let storage = storage_in(&from, &settings);
        storage.resume().unwrap();
        assert_eq!(storage.library(), from.join("library/icloud"));
        assert!(from.join("library/icloud/library.sqlite").exists());
        assert!(!to.join("library").exists());
        assert_eq!(read_settings(&settings).location, Some(from));
    }

    #[test]
    fn a_commit_that_cannot_be_written_undoes_the_move() {
        let dir = temp("no-commit");
        let (from, to) = (dir.join("old/library"), dir.join("new/library"));
        file(&from.join("icloud/library.sqlite"), 50, 0);
        std::fs::create_dir_all(to.parent().unwrap()).unwrap();
        let storage = storage_in(&dir.join("old"), &dir.join("settings.json"));
        let result = storage.move_library(&from, &to, || Err(anyhow!("the settings cannot be written")));
        assert!(result.is_err());
        // Back where it was, and in use there.
        assert!(from.join("icloud/library.sqlite").exists());
        assert!(!to.exists());
        assert_eq!(storage.library(), dir.join("old/library/icloud"));
        assert_eq!(storage.library_generation.load(Ordering::Relaxed), 0);
    }

    #[test]
    fn a_move_takes_the_library_in_use_when_it_moves_not_when_it_was_asked_for() {
        let dir = temp("move-switched");
        let (from, to) = (dir.join("old/library"), dir.join("new/library"));
        file(&from.join("icloud/library.sqlite"), 50, 0);
        file(&from.join("folder/library.sqlite"), 50, 0);
        std::fs::create_dir_all(to.parent().unwrap()).unwrap();
        let storage = storage_in(&dir.join("old"), &dir.join("settings.json"));
        // Asked for with iCloud's in use; the folder's shown before it goes.
        storage.switch_library(from.join("folder"));
        storage.move_library(&from, &to, || Ok(())).unwrap();
        assert_eq!(storage.library(), to.join("folder"));
    }

    #[test]
    fn a_new_limit_never_writes_back_the_folder_a_move_has_left() {
        let dir = temp("limit-during-move");
        let (from, to) = (dir.join("old"), dir.join("new"));
        file(&from.join("cache/full/aa/one.jpg"), 100, 0);
        let settings = dir.join("settings.json");
        let storage = storage_in(&from, &settings);
        // A new limit has read what is in force, and a move commits just
        // then, before the limit is written.
        let committing = {
            let storage = storage.clone();
            let (from, to) = (from.clone(), to.clone());
            move || {
                let committer = std::thread::spawn(move || storage.commit(&from, &to, &from.join("cache")).unwrap());
                // Give it every chance to commit now, if it can.
                std::thread::sleep(Duration::from_millis(200));
                drop(committer);
            }
        };
        *SAVE_BETWEEN.lock().unwrap() = Some(Box::new(committing));
        storage.set_limit(30 * GB).unwrap();
        // The commit came after the limit, not under it: the settings say
        // the new folder, as soon as it is in force.
        for _ in 0..100 {
            if storage.location() == to {
                break;
            }
            std::thread::sleep(Duration::from_millis(10));
        }
        assert_eq!(storage.location(), to);
        assert_eq!(read_settings(&settings).location, Some(to));
        assert_eq!(read_settings(&settings).limit, Some(30 * GB));
    }

    #[test]
    fn a_moved_library_opens_with_its_favourites() {
        let dir = temp("move-favourites");
        let (from, to) = (dir.join("old"), dir.join("new"));
        std::fs::create_dir_all(from.join("library/folder")).unwrap();
        {
            let db = crate::db::open(&from.join("library/folder/library.sqlite")).unwrap();
            db.execute_batch("CREATE TABLE folder_favorites (asset_id TEXT PRIMARY KEY); INSERT INTO folder_favorites VALUES ('IMG_1.JPG');")
                .unwrap();
        }
        let settings = dir.join("settings.json");
        let storage = Arc::new(Storage::new(from.clone(), from.join("library/folder"), from.join("cache"), None, settings.clone()));
        storage.set_location(to.clone()).unwrap();
        until_moved(&storage);
        assert_eq!(read_settings(&settings).location, Some(to.clone()));
        let db = crate::db::open(&storage.library().join("library.sqlite")).unwrap();
        let kept: String = db.query_row("SELECT asset_id FROM folder_favorites", [], |r| r.get(0)).unwrap();
        assert_eq!(kept, "IMG_1.JPG");
        assert!(storage.library().starts_with(&to));
    }

    #[test]
    fn a_resumed_move_that_can_be_neither_committed_nor_undone_stops_the_start() {
        use std::os::unix::fs::PermissionsExt;
        let dir = temp("resume-stuck");
        // The old folder cannot be made again, the settings not written.
        let locked = dir.join("locked");
        std::fs::create_dir_all(&locked).unwrap();
        let (from, to) = (locked.join("old"), dir.join("new"));
        file(&to.join("library/icloud/library.sqlite"), 50, 0);
        let conf = dir.join("conf");
        std::fs::create_dir_all(&conf).unwrap();
        let settings = conf.join("settings.json");
        std::fs::write(&settings, json!({"location": from, "moving": {"from": from, "to": to,
            "cache": from.join("cache"), "phase": "library"}}).to_string()).unwrap();
        for d in [&conf, &locked] {
            std::fs::set_permissions(d, std::fs::Permissions::from_mode(0o555)).unwrap();
        }
        let storage = storage_in(&from, &settings);
        assert!(storage.resume().is_err());
        for d in [&conf, &locked] {
            std::fs::set_permissions(d, std::fs::Permissions::from_mode(0o755)).unwrap();
        }
        // Nothing touched: the libraries where they are, none made old.
        assert!(to.join("library/icloud/library.sqlite").exists());
        assert!(!from.exists());
        // Started again, writable: committed, and in use there.
        let storage = storage_in(&from, &settings);
        storage.resume().unwrap();
        assert_eq!(storage.library(), to.join("library/icloud"));
        assert_eq!(read_settings(&settings).location, Some(to.clone()));
        until_moved(&storage);
        assert!(to.join("library/icloud/library.sqlite").exists());
    }

    #[test]
    fn a_resumed_move_whose_commit_cannot_be_written_is_undone_not_left_half() {
        use std::os::unix::fs::PermissionsExt;
        let dir = temp("resume-unwritable");
        let (from, to) = (dir.join("old"), dir.join("new"));
        file(&to.join("library/icloud/library.sqlite"), 50, 0);
        let conf = dir.join("conf");
        std::fs::create_dir_all(&conf).unwrap();
        let settings = conf.join("settings.json");
        std::fs::write(&settings, json!({"location": from, "moving": {"from": from, "to": to,
            "cache": from.join("cache"), "phase": "library"}}).to_string()).unwrap();
        // The settings can be read but not written.
        std::fs::set_permissions(&conf, std::fs::Permissions::from_mode(0o555)).unwrap();
        let storage = storage_in(&from, &settings);
        storage.resume().unwrap();
        std::fs::set_permissions(&conf, std::fs::Permissions::from_mode(0o755)).unwrap();
        // Back where the settings say, and nothing new made in its place.
        assert_eq!(storage.library(), from.join("library/icloud"));
        assert!(from.join("library/icloud/library.sqlite").exists());
        assert!(!to.join("library").exists());
        // Started again, with the settings writable: undone for good.
        let storage = storage_in(&from, &settings);
        storage.resume().unwrap();
        assert!(from.join("library/icloud/library.sqlite").exists());
        let written: Value = serde_json::from_str(&std::fs::read_to_string(&settings).unwrap()).unwrap();
        assert!(written.get("moving").is_none(), "{written:#}");
        assert_eq!(read_settings(&settings).location, Some(from));
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

    #[test]
    fn without_a_list_the_sources_are_the_apple_ids_then_the_folder_shown() {
        let accounts = vec!["a@x.com".to_owned(), "b@x.com".to_owned()];
        let folder = json!({"kind": "folder", "path": "/photos"});
        let s = resolve(&json!({"source": folder}), &accounts);
        assert_eq!(s.sources, vec![
            json!({"kind": "icloud", "account": "a@x.com"}),
            json!({"kind": "icloud", "account": "b@x.com"}),
            folder.clone(),
        ]);
        assert_eq!(s.source, Some(folder));
        // Nothing said: the first of them; nothing at all: none.
        assert_eq!(resolve(&json!({}), &accounts).source, Some(json!({"kind": "icloud", "account": "a@x.com"})));
        assert_eq!(resolve(&json!({}), &[]), Settings::default());
        // `null` is none, though there are sources.
        assert_eq!(resolve(&json!({"source": null}), &accounts).source, None);
        // An iCloud entry without an Apple ID is the only one signed in to.
        let one = resolve(&json!({"source": {"kind": "icloud"}}), &accounts[..1]);
        assert_eq!(one.source, Some(json!({"kind": "icloud", "account": "a@x.com"})));
    }

    #[test]
    fn a_list_is_kept_in_order_without_repeats() {
        let s = resolve(
            &json!({"sources": [
                {"kind": "folder", "path": "/b"}, {"kind": "icloud", "account": "a@x.com"},
                {"kind": "folder", "path": "/b"}, {"kind": "what"},
            ]}),
            &["z@x.com".to_owned()],
        );
        assert_eq!(s.sources, vec![json!({"kind": "folder", "path": "/b"}), json!({"kind": "icloud", "account": "a@x.com"})]);
        assert_eq!(s.source, Some(json!({"kind": "folder", "path": "/b"})));
        assert_eq!(source_id(&s.sources[1]), "icloud:a@x.com");
        assert_eq!(source_entry(&s, "folder:/b"), Some(json!({"kind": "folder", "path": "/b"})));
    }

    #[test]
    fn sources_are_added_shown_and_removed() {
        let dir = temp("sources");
        let path = dir.join("settings.json");
        std::fs::write(&path, r#"{"location": "/keep", "sources": [{"kind": "folder", "path": "/a"}]}"#).unwrap();
        let b = json!({"kind": "folder", "path": "/b"});
        show_source(&path, Some(&b)).unwrap();
        let s = read_settings(&path);
        assert_eq!(s.sources, vec![json!({"kind": "folder", "path": "/a"}), b.clone()]);
        assert_eq!(s.source, Some(b.clone()));
        assert_eq!(s.location, Some(PathBuf::from("/keep")));
        // Shown again: not added twice.
        show_source(&path, Some(&b)).unwrap();
        assert_eq!(read_settings(&path).sources.len(), 2);
        // The one shown goes: the first left is shown; then none.
        assert_eq!(remove_source(&path, "folder:/b").unwrap(), Some(json!({"kind": "folder", "path": "/a"})));
        assert_eq!(remove_source(&path, "folder:/a").unwrap(), None);
        let s = read_settings(&path);
        assert!(s.sources.is_empty());
        assert_eq!(s.source, None);
    }
}
