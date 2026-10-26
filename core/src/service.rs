//! pinholed: hands the window local files for any rendition of a photo,
//! from its cache or from the source, and keeps the library in step with
//! the source. It talks to the source only through the Source trait. See
//! docs/service.md and docs/sources.md.

use std::collections::HashMap;
use std::io::{BufRead, BufReader, Read, Write};
use std::os::unix::fs::PermissionsExt;
use std::os::unix::net::{UnixListener, UnixStream};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{mpsc, Arc, Mutex, RwLock};
use std::time::{Duration, SystemTime};

use anyhow::{anyhow, Context, Result};
use rusqlite::{params, Connection, OpenFlags};
use serde_json::{json, Value};

use crate::icloud::auth::{ProcessProvider, ProviderError};
use crate::source::{FetchError, Health, Source, Stored, Ticket, Want};
use crate::storage::Storage;
use crate::stream::Streams;
use crate::sync::thumb_path;

/// How long the lookup thread waits for more requests to batch with the
/// first, and how many it hands the source at once.
const BATCH_WINDOW: Duration = Duration::from_millis(25);
const BATCH_MAX: usize = 100;

pub struct Config {
    /// The folder everything is kept in (storage.rs), and in it, unless
    /// given otherwise, the library and the cache.
    pub location: PathBuf,
    pub library: PathBuf,
    pub cache: PathBuf,
    pub socket: PathBuf,
    /// Where the photos come from.
    pub source: Arc<dyn Source>,
    /// iCloud's sign-in provider, should the window switch to iCloud.
    pub auth: PathBuf,
    /// The limit for all of it, if one was set.
    pub limit: Option<u64>,
    /// Where the location and limit are kept when the window changes them.
    pub settings: PathBuf,
    /// A folder of photos from before (the old cache) to move in.
    pub adopt: Option<PathBuf>,
    pub downloads: usize,
    /// Seconds between rounds of following the source's changes, if not
    /// as often as the source would.
    pub sync_every: Option<u64>,
}

/// Where to answer one request.
#[derive(Clone)]
struct Reply {
    id: Value,
    out: Arc<Mutex<UnixStream>>,
    /// Answer as soon as the download starts, with a URL that plays it.
    stream: bool,
}

impl Reply {
    fn send(&self, result: std::result::Result<Value, (&str, String)>) {
        let line = match result {
            Ok(r) => json!({"id": self.id, "ok": true, "result": r}),
            Err((code, message)) => json!({"id": self.id, "ok": false, "error": {"code": code, "message": message}}),
        };
        if let Ok(mut out) = self.out.lock() {
            let _ = writeln!(out, "{line}");
            let _ = out.flush();
        }
    }
}

/// One file to bring into the cache.
#[derive(Clone)]
struct Job {
    stored: Stored,
    path: PathBuf,
    /// Once the source has got it ready.
    ticket: Option<Ticket>,
    retried: bool,
}

struct Shared {
    config: Config,
    source: RwLock<Arc<dyn Source>>,
    /// The cache's folder and limit, as they are now.
    storage: Arc<Storage>,
    lookups: Mutex<mpsc::Sender<Job>>,
    downloads: Mutex<mpsc::Sender<Job>>,
    /// Requests waiting on each file being fetched: a second request for the
    /// same file joins the first instead of downloading it again.
    waiting: Mutex<HashMap<PathBuf, (Value, Vec<Reply>)>>,
    in_flight: AtomicUsize,
    /// Every connected window, to tell when the library changed.
    listeners: Mutex<Vec<Arc<Mutex<UnixStream>>>>,
    /// Wakes the sync thread before its next round is due.
    sync_now: Mutex<mpsc::Sender<()>>,
    streams: Arc<Streams>,
    /// The play URL of each video downloading now, by its cache path.
    playable: Mutex<HashMap<PathBuf, String>>,
    /// Whether the source can be reached (and the session is good).
    health: Mutex<Health>,
    /// A library being built, as the last `progress` event said.
    progress: Mutex<Option<Value>>,
    /// Sign-ins waiting for their two-factor code, by Apple ID: the code
    /// has to reach the provider that started the sign-in.
    signing_in: Mutex<HashMap<String, ProcessProvider>>,
    /// Bumped as another source is shown, before its library is taken: a
    /// sync round of the one before sees it and gives up, so the switch
    /// does not wait for a library being built.
    shown: std::sync::atomic::AtomicU64,
    /// Held while the source shown changes (set_source, remove_source, a
    /// sign-in), so the settings and the source shown agree.
    changing: Mutex<()>,
    /// set_source and remove_source, done one after another in the order
    /// asked (keys pressed in a row: the last is the one shown).
    changes: Mutex<mpsc::Sender<(String, Value, Reply)>>,
    /// `shown` as the last sync round began: a round that sees it changed
    /// is the first since a source was shown, and finishes what an earlier
    /// round of it gave up (thumbnails still missing).
    rounds_shown: std::sync::atomic::AtomicU64,
}

fn health_event(health: Health) -> Value {
    json!({"event": "health", "state": health.name()})
}

/// What a window says about the source: its name, and whether it is one
/// that needs signing in. With none, `kind` is null.
fn source_info(source: &dyn Source) -> Value {
    if source.kind() == "none" {
        return json!({"kind": null, "name": "", "needs_sign_in": false});
    }
    let mut info = json!({"kind": source.kind(), "name": source.name(), "needs_sign_in": source.needs_sign_in()});
    if let Some(path) = source.id().strip_prefix("folder:") {
        info["path"] = json!(path);
    }
    if let Some(account) = source.id().strip_prefix("icloud:").filter(|a| !a.is_empty()) {
        info["account"] = json!(account);
    }
    info
}

fn source_event(source: &dyn Source) -> Value {
    let mut event = source_info(source);
    event["event"] = json!("source");
    event
}

/// How many photos and videos a library has, and how many of them have
/// their thumbnail; nothing when there is no library.
fn library_counts(library: &Path) -> (i64, i64) {
    let Ok(db) = Connection::open_with_flags(library.join("library.sqlite"), OpenFlags::SQLITE_OPEN_READ_ONLY) else {
        return (0, 0);
    };
    db.query_row("SELECT count(*), count(thumb_path) FROM assets", [], |r| Ok((r.get(0)?, r.get(1)?)))
        .unwrap_or((0, 0))
}

/// Every source added, in order, with what is in its library, and the
/// one shown (docs/service.md, `sources`).
fn sources_report(shared: &Shared) -> Value {
    let settings = crate::storage::read_settings(&shared.config.settings);
    let location = shared.storage.location();
    let current = shared.source().id();
    let sources: Vec<Value> = settings
        .sources
        .iter()
        .filter_map(|entry| {
            let source = crate::sync::source_for(Some(entry), shared.config.auth.clone()).ok()?;
            let id = source.id();
            let library = if id == current { shared.storage.library() } else { crate::storage::library_for(&location, &*source) };
            let (count, thumbs) = library_counts(&library);
            let mut info = json!({"id": id, "kind": source.kind(), "count": count, "thumbs": thumbs});
            if let Some(account) = entry["account"].as_str() {
                info["account"] = json!(account);
                info["name"] = json!(crate::icloud::auth::account_name(account).unwrap_or_else(|| account.to_owned()));
            } else {
                info["path"] = entry["path"].clone();
                info["name"] = json!(source.name());
            }
            Some(info)
        })
        .collect();
    json!({"sources": sources, "current": (current != "none").then_some(current)})
}

fn sources_event(shared: &Shared) -> Value {
    let mut event = sources_report(shared);
    event["event"] = json!("sources");
    event
}

/// A failed request's error code and message: a sign-in provider's own
/// code passes through.
fn failure(e: &anyhow::Error, otherwise: &'static str) -> (String, String) {
    match e.downcast_ref::<ProviderError>() {
        Some(p) => (p.code.clone(), p.message.clone()),
        None => (otherwise.to_owned(), format!("{e:#}")),
    }
}

/// Show another source: one added before, by its `id`, or a new one by
/// `kind` and its Apple ID or folder, added first. Kept in the settings,
/// the library switched to that source's own (made from scratch if it has
/// none yet), and every window told to read it.
fn set_source(shared: &Arc<Shared>, params: &Value) -> std::result::Result<Value, (String, String)> {
    let bad = |m: String| ("bad_request".to_owned(), m);
    let entry = match params["id"].as_str() {
        Some(id) => crate::storage::source_entry(&crate::storage::read_settings(&shared.config.settings), id)
            .ok_or_else(|| bad(format!("no source {id}")))?,
        None => match params["kind"].as_str() {
            Some("folder") => {
                let path = params["path"].as_str().ok_or_else(|| bad("a folder needs its path".into()))?;
                let path = path.trim_end_matches('/');
                if !Path::new(path).is_absolute() || !Path::new(path).is_dir() {
                    return Err(bad(format!("{path} is not a folder")));
                }
                json!({"kind": "folder", "path": if path.is_empty() { "/" } else { path }})
            }
            Some("icloud") => {
                let account = params["account"].as_str().ok_or_else(|| bad("iCloud needs an Apple ID".into()))?;
                if !crate::icloud::auth::accounts().iter().any(|a| a == account) {
                    return Err(("needs_sign_in".into(), format!("{account} is not signed in to")));
                }
                json!({"kind": "icloud", "account": account})
            }
            other => return Err(bad(format!("unknown source {other:?}"))),
        },
    };
    let source = crate::sync::source_for(Some(&entry), shared.config.auth.clone()).map_err(|e| bad(format!("{e:#}")))?;
    let _changing = shared.changing.lock().unwrap();
    crate::storage::show_source(&shared.config.settings, Some(&entry)).map_err(|e| bad(format!("{e:#}")))?;
    if source.id() != shared.source().id() {
        switch(shared, source.clone()).map_err(|e| bad(format!("{e:#}")))?;
    }
    shared.broadcast(sources_event(shared));
    Ok(source_info(&*source))
}

/// Take a source out: its library deleted (never the photos themselves),
/// an Apple ID's session too, and if it was the one shown, the first left
/// shown instead, or none.
fn remove_source(shared: &Arc<Shared>, params: &Value) -> std::result::Result<Value, (String, String)> {
    let bad = |m: String| ("bad_request".to_owned(), m);
    let id = params["id"].as_str().ok_or_else(|| bad("remove_source needs an id".into()))?;
    let _changing = shared.changing.lock().unwrap();
    let settings = crate::storage::read_settings(&shared.config.settings);
    let entry = crate::storage::source_entry(&settings, id).ok_or_else(|| bad(format!("no source {id}")))?;
    let source = crate::sync::source_for(Some(&entry), shared.config.auth.clone()).map_err(|e| bad(format!("{e:#}")))?;
    let shown = crate::storage::remove_source(&shared.config.settings, id).map_err(|e| bad(format!("{e:#}")))?;
    if shared.source().id() == id {
        let next = crate::sync::source_for(shown.as_ref(), shared.config.auth.clone()).map_err(|e| bad(format!("{e:#}")))?;
        switch(shared, next).map_err(|e| bad(format!("{e:#}")))?;
    }
    if let Some(account) = entry["account"].as_str() {
        shared.signing_in.lock().unwrap().remove(account);
        let _ = std::fs::remove_dir_all(crate::icloud::auth::state_dir(account));
    }
    // Only ever a folder under library/, never the library shown now.
    let library = crate::storage::library_for(&shared.storage.location(), &*source);
    let libraries = shared.storage.location().join("library");
    if library.starts_with(&libraries) && library != libraries && library != shared.storage.library() {
        let _writing = shared.storage.library_lock.lock().unwrap();
        if let Err(e) = std::fs::remove_dir_all(&library) {
            if e.kind() != std::io::ErrorKind::NotFound {
                eprintln!("pinholed: removing {}: {e}", library.display());
            }
        }
    }
    eprintln!("pinholed: removed {id}");
    shared.broadcast(sources_event(shared));
    Ok(sources_report(shared))
}

/// Sign out of an Apple ID: its session deleted, nothing else. It stays
/// among the sources with its library (and its owner's name), which shows
/// what is on this computer until it is signed in to again, at once.
fn sign_out(shared: &Arc<Shared>, params: &Value) -> std::result::Result<Value, (String, String)> {
    let bad = |m: String| ("bad_request".to_owned(), m);
    let id = params["id"].as_str().ok_or_else(|| bad("sign_out needs an id".into()))?;
    let _changing = shared.changing.lock().unwrap();
    let settings = crate::storage::read_settings(&shared.config.settings);
    let entry = crate::storage::source_entry(&settings, id).ok_or_else(|| bad(format!("no source {id}")))?;
    let account = entry["account"].as_str().ok_or_else(|| bad(format!("{id} is not an Apple ID")))?;
    shared.signing_in.lock().unwrap().remove(account);
    // The provider keeps the whole session there (docs: auth/PROTOCOL.md).
    let cookies = crate::icloud::auth::state_dir(account).join("cookies");
    if let Err(e) = std::fs::remove_dir_all(&cookies) {
        if e.kind() != std::io::ErrorKind::NotFound {
            return Err(("unavailable".into(), format!("removing {}: {e}", cookies.display())));
        }
    }
    // Shown: a fresh source, which has no session to go on.
    if shared.source().id() == id {
        let source = crate::sync::source_for(Some(&entry), shared.config.auth.clone()).map_err(|e| bad(format!("{e:#}")))?;
        switch(shared, source).map_err(|e| bad(format!("{e:#}")))?;
    }
    eprintln!("pinholed: signed out of {account}");
    shared.broadcast(sources_event(shared));
    Ok(sources_report(shared))
}

/// What could be added (docs/service.md, `discover`).
fn discover(shared: &Shared) -> Value {
    let home = PathBuf::from(std::env::var_os("HOME").unwrap_or_default());
    let config = std::env::var_os("XDG_CONFIG_HOME").map(PathBuf::from).unwrap_or_else(|| home.join(".config"));
    let user = std::env::var("USER").unwrap_or_else(|_| home.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default());
    let settings = crate::storage::read_settings(&shared.config.settings);
    let added: std::collections::HashSet<PathBuf> =
        settings.sources.iter().filter_map(|e| e["path"].as_str()).map(PathBuf::from).collect();
    // Where icloudpd keeps its sessions, as the provider's import reads them.
    let stems = crate::discover::icloudpd_stems(&home.join(".config/icloudpd"), &crate::icloud::auth::accounts());
    let pictures = crate::discover::pictures_dir(&home, &config);
    let candidates = crate::discover::candidates(&pictures, &Path::new("/run/media").join(user));
    crate::discover::discover(stems, candidates, &added, &home, crate::discover::BUDGET)
}

/// Sign in to an Apple ID with its password, or without one by taking
/// over icloudpd's session for it. Signed in, it is added and shown.
fn sign_in(shared: &Arc<Shared>, params: &Value) -> std::result::Result<Value, (String, String)> {
    let apple_id = params["apple_id"].as_str().map(str::trim).filter(|a| !a.is_empty())
        .ok_or_else(|| ("bad_request".to_owned(), "sign_in needs an apple_id".to_owned()))?;
    let password = params["password"].as_str().filter(|p| !p.is_empty());
    let state_dir = crate::icloud::auth::state_dir(apple_id);
    let known = state_dir.join("account.json").exists();
    // A sign-in that fails leaves no trace of an Apple ID that was not
    // signed in to before.
    let forget = |e: (String, String)| {
        if !known {
            let _ = std::fs::remove_dir_all(&state_dir);
        }
        e
    };
    shared.signing_in.lock().unwrap().remove(apple_id);
    let mut provider = ProcessProvider::spawn(shared.config.auth.clone(), Some(state_dir.clone()))
        .map_err(|e| forget(failure(&e, "unavailable")))?;
    match password {
        None => {
            provider.call("import_icloudpd", json!({"apple_id": apple_id})).map_err(|e| forget(failure(&e, "unavailable")))?;
            let status = provider.call("status", Value::Null).map_err(|e| forget(failure(&e, "unavailable")))?;
            if status["state"] != "signed_in" {
                return Err(forget(("needs_sign_in".into(), format!("icloudpd's session for {apple_id} is no longer valid"))));
            }
        }
        Some(password) => {
            let state = provider
                .call("sign_in", json!({"apple_id": apple_id, "password": password}))
                .map_err(|e| forget(failure(&e, "unavailable")))?;
            if state["state"] == "needs_code" {
                shared.signing_in.lock().unwrap().insert(apple_id.to_owned(), provider);
                return Ok(json!({"state": "needs_code"}));
            }
        }
    }
    signed_in(shared, apple_id)
}

/// The two-factor code for a sign-in waiting for it. A wrong code can be
/// tried again.
fn submit_code(shared: &Arc<Shared>, params: &Value) -> std::result::Result<Value, (String, String)> {
    let bad = |m: &str| ("bad_request".to_owned(), m.to_owned());
    let apple_id = params["apple_id"].as_str().ok_or_else(|| bad("submit_code needs an apple_id"))?;
    let code = params["code"].as_str().map(str::to_owned).or_else(|| params["code"].as_u64().map(|c| format!("{c:06}")))
        .ok_or_else(|| bad("submit_code needs the code"))?;
    let mut provider = shared.signing_in.lock().unwrap().remove(apple_id)
        .ok_or_else(|| bad("no sign-in is waiting for a code for that Apple ID"))?;
    if let Err(e) = provider.call("submit_code", json!({"code": code})) {
        let (code, message) = failure(&e, "unavailable");
        if code == "wrong_code" {
            shared.signing_in.lock().unwrap().insert(apple_id.to_owned(), provider);
        }
        return Err((code, message));
    }
    drop(provider);
    signed_in(shared, apple_id)
}

/// An Apple ID signed in to: added to the sources and shown, from a fresh
/// source so its new session is used even when it was shown already.
fn signed_in(shared: &Arc<Shared>, apple_id: &str) -> std::result::Result<Value, (String, String)> {
    let entry = json!({"kind": "icloud", "account": apple_id});
    let fail = |e: anyhow::Error| ("unavailable".to_owned(), format!("{e:#}"));
    let _changing = shared.changing.lock().unwrap();
    crate::storage::show_source(&shared.config.settings, Some(&entry)).map_err(fail)?;
    let source = crate::sync::source_for(Some(&entry), shared.config.auth.clone()).map_err(fail)?;
    switch(shared, source).map_err(fail)?;
    eprintln!("pinholed: signed in to {apple_id}");
    shared.broadcast(sources_event(shared));
    Ok(json!({"state": "signed_in"}))
}

/// Photos from `source` from now on: the library switched to its own (made
/// if it has none yet), and every window told to read it.
fn switch(shared: &Arc<Shared>, source: Arc<dyn Source>) -> Result<()> {
    shared.shown.fetch_add(1, Ordering::SeqCst);
    {
        // Not while a sync round writes the library: one building it gives
        // up within a second or so, told by `shown`.
        let _writing = shared.storage.library_lock.lock().unwrap();
        *shared.source.write().unwrap() = source.clone();
        *shared.health.lock().unwrap() = Health::Fine;
        *shared.progress.lock().unwrap() = None;
        let shown = if source.kind() == "none" { "nowhere".to_owned() } else { source.id() };
        eprintln!("pinholed: photos from {shown} now");
        shared.broadcast(source_event(&*source));
        shared.broadcast(health_event(Health::Fine));
        // There before windows are told to read it, if only empty.
        let library = crate::storage::library_for(&shared.storage.location(), &*source);
        std::fs::create_dir_all(&library)?;
        crate::db::open(&library.join("library.sqlite"))?;
        shared.storage.switch_library(library);
    }
    let _ = shared.sync_now.lock().unwrap().send(());
    if let Err(e) = source.start() {
        shared.failed(&e);
    }
    Ok(())
}

/// The source the settings name, if pinhole-sync changed it (signing in to
/// an Apple ID makes it the source): switched to.
fn follow_settings(shared: &Arc<Shared>) {
    // A window changing it now: its own change is under way.
    let Ok(_changing) = shared.changing.try_lock() else { return };
    let settings = crate::storage::read_settings(&shared.config.settings);
    let Ok(source) = crate::sync::source_for(settings.source.as_ref(), shared.config.auth.clone()) else { return };
    if source.id() != shared.source().id() {
        if let Err(e) = switch(shared, source) {
            eprintln!("pinholed: switching source: {e:#}");
        }
        shared.broadcast(sources_event(shared));
    }
}

impl Shared {
    fn source(&self) -> Arc<dyn Source> {
        self.source.read().unwrap().clone()
    }

    /// Note how the last exchange with the source went; windows hear of
    /// changes.
    fn set_health(&self, health: Health) {
        let mut current = self.health.lock().unwrap();
        if *current == health {
            return;
        }
        eprintln!("pinholed: {}: {}", self.source().name(), health.name());
        *current = health;
        drop(current);
        self.broadcast(health_event(health));
    }

    /// What an error from the source says, noted; the error code a request
    /// it failed is answered with.
    fn failed(&self, error: &anyhow::Error) -> &'static str {
        let health = self.source().health(error);
        self.set_health(health);
        if health == Health::NeedsSignIn { "needs_sign_in" } else { "unavailable" }
    }

    /// A download worked: the network is back, though that says nothing
    /// about the session.
    fn reached(&self) {
        if matches!(*self.health.lock().unwrap(), Health::Offline | Health::Failing) {
            self.set_health(Health::Fine);
        }
    }

    /// Send an event line to every window; ones that have gone are dropped.
    fn broadcast(&self, event: Value) {
        let line = format!("{event}\n");
        self.listeners.lock().unwrap().retain(|out| {
            let Ok(mut out) = out.lock() else { return false };
            out.write_all(line.as_bytes()).and_then(|_| out.flush()).is_ok()
        });
    }
}

impl Shared {
    /// A video started downloading: those who asked to stream it get its
    /// play URL now; the rest wait for the file.
    fn started(&self, path: &Path, url: &str) {
        self.playable.lock().unwrap().insert(path.to_owned(), url.to_owned());
        let mut waiting = self.waiting.lock().unwrap();
        let Some((info, replies)) = waiting.get_mut(path) else { return };
        let mut info = info.clone();
        info["stream"] = json!(url);
        for reply in std::mem::take(replies) {
            if reply.stream {
                reply.send(Ok(info.clone()));
            } else {
                replies.push(reply);
            }
        }
    }

    fn finish(&self, path: &Path, result: std::result::Result<(), (&str, String)>) {
        self.playable.lock().unwrap().remove(path);
        let Some((info, replies)) = self.waiting.lock().unwrap().remove(path) else { return };
        self.in_flight.fetch_sub(1, Ordering::Relaxed);
        for reply in replies {
            match &result {
                Ok(()) => reply.send(Ok(info.clone())),
                Err((code, message)) => reply.send(Err((code, message.clone()))),
            }
        }
    }
}

fn extension(file_type: &str) -> &'static str {
    match file_type {
        "public.jpeg" => "jpg",
        "public.heic" => "heic",
        "public.png" => "png",
        "com.apple.quicktime-movie" => "mov",
        "public.mpeg-4" => "mp4",
        "com.apple.m4v-video" => "m4v",
        "public.avi" => "avi",
        "com.compuserve.gif" => "gif",
        "public.tiff" => "tif",
        _ => "bin",
    }
}

fn touch(path: &Path) {
    if let Ok(f) = std::fs::File::options().write(true).open(path) {
        let _ = f.set_modified(SystemTime::now());
    }
}

pub fn run(config: Config) -> Result<()> {
    std::fs::create_dir_all(&config.cache)?;
    eprintln!("pinholed: photos from {}", if config.source.kind() == "none" { "nowhere".to_owned() } else { config.source.id() });
    let listener = bind(&config.socket)?;
    eprintln!("pinholed: listening on {}", config.socket.display());

    let (lookup_tx, lookup_rx) = mpsc::channel::<Job>();
    let (sync_tx, sync_rx) = mpsc::channel::<()>();
    let (download_tx, download_rx) = mpsc::channel::<Job>();
    let (changes_tx, changes_rx) = mpsc::channel::<(String, Value, Reply)>();
    let storage = Arc::new(Storage::new(
        config.location.clone(),
        config.library.clone(),
        config.cache.clone(),
        config.limit,
        config.settings.clone(),
    ));
    let shared = Arc::new(Shared {
        source: RwLock::new(config.source.clone()),
        config,
        storage,
        lookups: Mutex::new(lookup_tx),
        downloads: Mutex::new(download_tx),
        waiting: Mutex::new(HashMap::new()),
        in_flight: AtomicUsize::new(0),
        listeners: Mutex::new(Vec::new()),
        sync_now: Mutex::new(sync_tx),
        streams: Streams::start()?,
        playable: Mutex::new(HashMap::new()),
        health: Mutex::new(Health::Fine),
        progress: Mutex::new(None),
        signing_in: Mutex::new(HashMap::new()),
        shown: std::sync::atomic::AtomicU64::new(0),
        rounds_shown: std::sync::atomic::AtomicU64::new(u64::MAX),
        changing: Mutex::new(()),
        changes: Mutex::new(changes_tx),
    });

    {
        let shared = shared.clone();
        std::thread::spawn(move || lookup_loop(shared, lookup_rx));
    }
    let download_rx = Arc::new(Mutex::new(download_rx));
    for _ in 0..shared.config.downloads.max(1) {
        let (shared, rx) = (shared.clone(), download_rx.clone());
        std::thread::spawn(move || download_loop(shared, rx));
    }
    {
        let storage = shared.storage.clone();
        std::thread::spawn(move || storage.evict_loop());
    }
    {
        if let Some(old) = shared.config.adopt.clone() {
            shared.storage.adopt(old);
        }
        // Every window reads the library from its new place.
        let weak = Arc::downgrade(&shared);
        shared.storage.on_library_moved(move |library| {
            if let Some(shared) = weak.upgrade() {
                shared.broadcast(json!({"event": "library_moved", "library": library}));
            }
        });
    }
    {
        let shared = shared.clone();
        std::thread::spawn(move || sync_loop(shared, sync_rx));
    }
    {
        let shared = shared.clone();
        std::thread::spawn(move || {
            for (method, params, reply) in changes_rx {
                let result = match method.as_str() {
                    "set_source" => set_source(&shared, &params),
                    "sign_out" => sign_out(&shared, &params),
                    _ => remove_source(&shared, &params),
                };
                match result {
                    Ok(value) => reply.send(Ok(value)),
                    Err((code, message)) => reply.send(Err((&code, message))),
                }
            }
        });
    }

    for stream in listener.incoming() {
        let Ok(stream) = stream else { continue };
        let shared = shared.clone();
        std::thread::spawn(move || {
            if let Err(e) = serve(shared, stream) {
                eprintln!("pinholed: connection ended: {e:#}");
            }
        });
    }
    Ok(())
}

/// Bind the socket, replacing one left behind by a service that is gone,
/// but never one that still answers.
fn bind(socket: &Path) -> Result<UnixListener> {
    if socket.exists() {
        if UnixStream::connect(socket).is_ok() {
            return Err(anyhow!("another pinholed is already running on {}", socket.display()));
        }
        std::fs::remove_file(socket)?;
    }
    if let Some(dir) = socket.parent() {
        std::fs::create_dir_all(dir)?;
    }
    let listener = UnixListener::bind(socket).with_context(|| format!("binding {}", socket.display()))?;
    std::fs::set_permissions(socket, std::fs::Permissions::from_mode(0o600))?;
    Ok(listener)
}

fn serve(shared: Arc<Shared>, stream: UnixStream) -> Result<()> {
    let out = Arc::new(Mutex::new(stream.try_clone()?));
    shared.listeners.lock().unwrap().push(out.clone());
    // A window that connects hears where the photos come from, and if
    // something is wrong, that too.
    let _ = writeln!(out.lock().unwrap(), "{}", source_event(&*shared.source()));
    let _ = writeln!(out.lock().unwrap(), "{}", sources_event(&shared));
    let health = *shared.health.lock().unwrap();
    if health != Health::Fine {
        let _ = writeln!(out.lock().unwrap(), "{}", health_event(health));
    }
    if let Some(progress) = shared.progress.lock().unwrap().clone() {
        let _ = writeln!(out.lock().unwrap(), "{progress}");
    }
    // Opened when first needed: before pinhole-sync has built the library
    // there is none, and the connection must stay up regardless.
    // Opened again once the library has moved.
    let mut db: Option<Connection> = None;
    let mut db_generation = shared.storage.library_generation.load(Ordering::Relaxed);
    for line in BufReader::new(stream).lines() {
        let line = line?;
        if line.trim().is_empty() {
            continue;
        }
        let request: Value = match serde_json::from_str(&line) {
            Ok(v) => v,
            Err(_) => {
                Reply { id: Value::Null, out: out.clone(), stream: false }.send(Err(("bad_request", "not JSON".into())));
                continue;
            }
        };
        let stream = request["params"]["stream"].as_bool() == Some(true);
        let reply = Reply { id: request["id"].clone(), out: out.clone(), stream };
        match request["method"].as_str() {
            Some("ping") => reply.send(Ok(json!({}))),
            Some("sync") => {
                let _ = shared.sync_now.lock().unwrap().send(());
                reply.send(Ok(json!({})))
            }
            Some("status") => reply.send(Ok(json!({
                "library": shared.storage.library(),
                "cache": shared.storage.cache(),
                "cache_bytes": shared.storage.report()["used_bytes"],
                "in_flight": shared.in_flight.load(Ordering::Relaxed),
                "health": shared.health.lock().unwrap().name(),
                "source": source_info(&*shared.source()),
                "progress": shared.progress.lock().unwrap().clone().map(|mut p| {
                    p.as_object_mut().map(|o| o.remove("event"));
                    p
                }),
            }))),
            Some("sources") => reply.send(Ok(sources_report(&shared))),
            // A switch may wait a moment for a library being built to
            // give up: done in turn, off this connection's thread.
            Some(method @ ("set_source" | "remove_source" | "sign_out")) => {
                let _ = shared.changes.lock().unwrap().send((method.to_owned(), request["params"].clone(), reply));
            }
            // These take a while (the sign-in provider starts, Apple
            // answers, a disk is looked through): on a thread of their
            // own, so this window's photos keep coming meanwhile.
            Some(method @ ("sign_in" | "submit_code" | "discover")) => {
                let (shared, params, method) = (shared.clone(), request["params"].clone(), method.to_owned());
                std::thread::spawn(move || {
                    let result = match method.as_str() {
                        "sign_in" => sign_in(&shared, &params),
                        "submit_code" => submit_code(&shared, &params),
                        _ => Ok(discover(&shared)),
                    };
                    match result {
                        Ok(value) => reply.send(Ok(value)),
                        Err((code, message)) => reply.send(Err((&code, message))),
                    }
                });
            }
            Some("storage") => reply.send(Ok(shared.storage.report())),
            Some("set_storage") => {
                let params = &request["params"];
                let mut result = Ok(());
                if let Some(limit) = params["limit_bytes"].as_u64() {
                    result = shared.storage.set_limit(limit);
                }
                if let (Ok(()), Some(location)) = (&result, params["location"].as_str()) {
                    result = shared.storage.set_location(PathBuf::from(location));
                }
                match result {
                    Ok(()) => reply.send(Ok(shared.storage.report())),
                    Err(e) => reply.send(Err(("bad_request", format!("{e:#}")))),
                }
            }
            Some("fetch") => {
                let generation = shared.storage.library_generation.load(Ordering::Relaxed);
                if db.is_none() || generation != db_generation {
                    db_generation = generation;
                    db = Connection::open_with_flags(
                        shared.storage.library().join("library.sqlite"),
                        OpenFlags::SQLITE_OPEN_READ_ONLY | OpenFlags::SQLITE_OPEN_NO_MUTEX,
                    )
                    .ok();
                }
                match &db {
                    Some(db) => fetch(&shared, db, &request["params"], reply),
                    None => reply.send(Err(("not_found", "there is no library yet".into()))),
                }
            }
            other => reply.send(Err(("bad_request", format!("unknown method {other:?}")))),
        }
    }
    Ok(())
}

fn fetch(shared: &Shared, db: &Connection, params: &Value, reply: Reply) {
    let (Some(asset), Some(kind)) = (params["asset"].as_str(), params["kind"].as_str()) else {
        return reply.send(Err(("bad_request", "fetch needs asset and kind".into())));
    };
    let Some(want) = Want::parse(kind).filter(|w| *w != Want::Thumb) else {
        return reply.send(Err(("bad_request", format!("unknown kind {kind}"))));
    };

    // The rendition that serves this kind, as the catalog chose it.
    let row = db.query_row(
        "SELECT name, key, coalesce(file_type, '') FROM renditions WHERE asset_id = ?1 AND kind = ?2",
        params![asset, kind],
        |r| Ok(Stored { asset: asset.to_owned(), want, name: r.get(0)?, key: r.get(1)?, file_type: r.get(2)? }),
    );
    let Ok(stored) = row else {
        return reply.send(Err(("not_found", format!("{asset} has no {kind} rendition"))));
    };
    let (rendition, file_type) = (stored.name.clone(), stored.file_type.clone());
    // On this computer already: served as it is.
    if let Some(path) = shared.source().local(&stored) {
        return reply.send(if path.exists() {
            Ok(json!({"path": path, "type": file_type, "rendition": rendition, "cached": true}))
        } else {
            Err(("not_found", format!("{} is not there", path.display())))
        });
    }
    let relative = Path::new(kind).join(
        thumb_path(&stored.key).replacen("thumbs/", "", 1).replace(".jpg", &format!(".{}", extension(&file_type))),
    );
    if let Some(path) = shared.storage.find(&relative) {
        touch(&path);
        return reply.send(Ok(json!({"path": path, "type": file_type, "rendition": rendition, "cached": true})));
    }
    let path = shared.storage.cache().join(&relative);
    let info = json!({"path": path, "type": file_type, "rendition": rendition, "cached": true});

    let mut waiting = shared.waiting.lock().unwrap();
    if let Some((info, replies)) = waiting.get_mut(&path) {
        match shared.playable.lock().unwrap().get(&path) {
            Some(url) if reply.stream => {
                let mut info = info.clone();
                info["stream"] = json!(url);
                reply.send(Ok(info));
            }
            _ => replies.push(reply),
        }
        return;
    }
    let mut info = info;
    info["cached"] = json!(false);
    waiting.insert(path.clone(), (info, vec![reply]));
    shared.in_flight.fetch_add(1, Ordering::Relaxed);
    drop(waiting);

    let _ = shared.lookups.lock().unwrap().send(Job { stored, path, ticket: None, retried: false });
}

/// Gathers requests for a moment and has the source get them all ready in
/// one go (iCloud looks their links up together), then passes them to the
/// downloads.
fn lookup_loop(shared: Arc<Shared>, rx: mpsc::Receiver<Job>) {
    // Ready at start: iCloud's provider takes a few seconds to check the
    // session, which the first request should not have to wait for.
    if let Err(e) = shared.source().start() {
        eprintln!("pinholed: {} is not ready yet: {e:#}", shared.source().name());
        shared.failed(&e);
    }
    let mut db: Option<Connection> = None;
    let mut db_generation = 0;
    while let Ok(first) = rx.recv() {
        std::thread::sleep(BATCH_WINDOW);
        let mut batch = vec![first];
        while batch.len() < BATCH_MAX {
            match rx.try_recv() {
                Ok(job) => batch.push(job),
                Err(_) => break,
            }
        }
        // The source may keep what it learns in the index (iCloud its fresh
        // links), so the next request needs no lookup.
        let generation = shared.storage.library_generation.load(Ordering::Relaxed);
        if db.is_none() || generation != db_generation {
            db_generation = generation;
            db = crate::db::open(&shared.storage.library().join("library.sqlite")).ok();
        }
        let Some(db) = &db else {
            for job in batch {
                shared.finish(&job.path, Err(("not_found", "there is no library yet".into())));
            }
            continue;
        };
        let stored: Vec<Stored> = batch.iter().map(|j| j.stored.clone()).collect();
        let tickets = match shared.source().prepare(db, &stored) {
            Ok(t) => t,
            Err(e) => {
                let code = shared.failed(&e);
                for job in batch {
                    shared.finish(&job.path, Err((code, format!("{e:#}"))));
                }
                continue;
            }
        };
        shared.set_health(Health::Fine);
        for (mut job, ticket) in batch.into_iter().zip(tickets) {
            match ticket {
                Ok(ticket) => {
                    job.ticket = Some(ticket);
                    let _ = shared.downloads.lock().unwrap().send(job);
                }
                Err(FetchError::Failed(e)) => {
                    let code = shared.failed(&e);
                    shared.finish(&job.path, Err((code, format!("{e:#}"))))
                }
                Err(e) => shared.finish(&job.path, Err(("not_found", e.to_string()))),
            }
        }
    }
}

fn download_loop(shared: Arc<Shared>, rx: Arc<Mutex<mpsc::Receiver<Job>>>) {
    loop {
        let job = { rx.lock().unwrap().recv() };
        let Ok(job) = job else { return };
        match download(&shared, &job) {
            Ok(()) => {
                shared.reached();
                shared.finish(&job.path, Ok(()))
            }
            // A stale ticket (an expired link): got ready again, once.
            Err(FetchError::Expired) if !job.retried => {
                let mut again = job.clone();
                again.ticket = None;
                again.retried = true;
                let _ = shared.lookups.lock().unwrap().send(again);
            }
            Err(FetchError::Expired) => shared.finish(&job.path, Err(("unavailable", "the download link kept expiring".into()))),
            Err(FetchError::NotFound(e)) => shared.finish(&job.path, Err(("not_found", e))),
            Err(FetchError::Failed(e)) => {
                let code = shared.failed(&e);
                shared.finish(&job.path, Err((code, format!("{e:#}"))))
            }
        }
    }
}

fn download(shared: &Shared, job: &Job) -> std::result::Result<(), FetchError> {
    let source = shared.source();
    let ticket = job.ticket.clone().ok_or_else(|| FetchError::Failed(anyhow!("not got ready")))?;
    let mut body = source.open(&ticket, None)?;
    if let Some(dir) = job.path.parent() {
        std::fs::create_dir_all(dir).map_err(|e| FetchError::Failed(e.into()))?;
    }
    // Written beside the target and renamed: a reader never sees half a file.
    let part = job.path.with_extension("part");
    let mut stream = None;
    let result = (|| -> std::io::Result<()> {
        // Readable too: a stream serves from this same handle.
        let mut file = std::fs::File::options().read(true).write(true).create(true).truncate(true).open(&part)?;
        if job.stored.want.streamable() {
            let (source, ticket) = (source.clone(), ticket.clone());
            let ranged: crate::stream::Ranged = Box::new(move |start, end| {
                source.open(&ticket, Some((start, end))).map(|b| b.reader).map_err(|e| anyhow!("{e}"))
            });
            let (url, s) = shared.streams.add(file.try_clone()?, body.length, &job.stored.file_type, ranged);
            shared.started(&job.path, &url);
            stream = Some(s);
        }
        let mut buf = vec![0u8; 256 << 10];
        loop {
            let n = body.reader.read(&mut buf)?;
            if n == 0 {
                break;
            }
            file.write_all(&buf[..n])?;
            if let Some(s) = &stream {
                s.advance(n as u64);
            }
        }
        file.sync_data()?;
        std::fs::rename(&part, &job.path)
    })();
    if let Some(s) = &stream {
        s.finish(result.is_ok());
    }
    if let Err(e) = result {
        let _ = std::fs::remove_file(&part);
        return Err(FetchError::Failed(e.into()));
    }
    Ok(())
}

/// Follows the source's changes: every `sync_every` seconds, or when a
/// window asks, it applies what changed (listing the whole library when
/// there is nothing to follow from yet), makes thumbnails for anything new,
/// and tells every window that the library changed (only when something it
/// draws did).
fn sync_loop(shared: Arc<Shared>, wake: mpsc::Receiver<()>) {
    loop {
        follow_settings(&shared);
        match sync_round(&shared) {
            Ok(()) => shared.set_health(Health::Fine),
            Err(e) => {
                eprintln!("pinholed: sync: {e:#}");
                shared.failed(&e);
            }
        }
        // Until the next round, or a window asking for one now.
        let every = shared.config.sync_every.map_or_else(|| shared.source().rounds_every(), Duration::from_secs);
        match wake.recv_timeout(every.max(Duration::from_secs(5))) {
            Ok(()) | Err(mpsc::RecvTimeoutError::Timeout) => {}
            Err(mpsc::RecvTimeoutError::Disconnected) => return,
        }
        // Several asks at once make one round.
        while wake.try_recv().is_ok() {}
    }
}

/// Tells windows how a library being built is coming along: `progress`
/// at most four times a second, the library itself every four seconds so
/// the grid fills as it does, and `done` at the end if anything was said.
struct Building<'a> {
    shared: &'a Shared,
    source: String,
    said: Mutex<(Option<std::time::Instant>, std::time::Instant)>,
}

impl<'a> Building<'a> {
    fn new(shared: &'a Shared, source: String) -> Self {
        Building { shared, source, said: Mutex::new((None, std::time::Instant::now())) }
    }

    fn step(&self, phase: &str, done: u64, total: u64) {
        let mut said = self.said.lock().unwrap();
        if said.0.is_some_and(|t| t.elapsed() < Duration::from_millis(250)) {
            return;
        }
        said.0 = Some(std::time::Instant::now());
        let event = json!({"event": "progress", "source": self.source, "phase": phase, "done": done, "total": total});
        *self.shared.progress.lock().unwrap() = Some(event.clone());
        self.shared.broadcast(event);
        if said.1.elapsed() > Duration::from_secs(4) {
            said.1 = std::time::Instant::now();
            self.shared.broadcast(json!({"event": "library_changed", "version": null, "added": 0, "removed": 0}));
            self.shared.broadcast(sources_event(self.shared));
        }
    }

    fn finish(&self) {
        if self.said.lock().unwrap().0.is_none() {
            return;
        }
        *self.shared.progress.lock().unwrap() = None;
        self.shared.broadcast(json!({"event": "progress", "source": self.source, "phase": "done"}));
    }
}

fn sync_round(shared: &Shared) -> Result<()> {
    // Read before the source: a switch after this stops the round.
    let shown = shared.shown.load(Ordering::SeqCst);
    let stop = || shared.shown.load(Ordering::SeqCst) != shown;
    let source = shared.source();
    if source.kind() == "none" {
        return Ok(());
    }
    // Nothing is made for a source not ready, such as an iCloud with no
    // Apple ID signed in to yet.
    source.start()?;
    // Not while the library moves.
    let _writing = shared.storage.library_lock.lock().unwrap();
    let library = shared.storage.library();
    std::fs::create_dir_all(&library)?;
    let mut db = crate::db::open(&library.join("library.sqlite"))?;
    crate::sync::claim(source.as_ref(), &mut db, &library)?;
    let first = shared.rounds_shown.swap(shown, Ordering::SeqCst) != shown;
    let building = Building::new(shared, source.id());
    let result = (|| -> Result<()> {
        let report = crate::sync::catch_up(source.as_ref(), &mut db, &|done, total| building.step("index", done, total), &stop)?;
        let (added, removed) = (report.changes.added.len(), report.changes.removed.len());
        // Thumbnails for what is new, and on the first round since the
        // source was shown, for whatever an earlier round left without
        // one (it gave up when another source was shown). Not every round:
        // a photo that cannot be made into one is not tried every minute.
        let missing: i64 = if added > 0 || first {
            db.query_row(
                "SELECT count(*) FROM assets a JOIN renditions r ON r.asset_id = a.id AND r.kind = 'thumb'
                 WHERE a.thumb_path IS NULL",
                [],
                |r| r.get(0),
            )?
        } else {
            0
        };
        if !report.changes.visible() && missing == 0 {
            return Ok(());
        }
        let version: i64 = crate::db::meta(&db, "library_version").and_then(|v| v.parse().ok()).unwrap_or(0);
        let announce = |added: usize, removed: usize| {
            shared.broadcast(json!({"event": "library_changed", "version": version, "added": added, "removed": removed}));
        };
        if missing > 0 {
            // What was listed shows at once, as it waits for its
            // thumbnails, rather than when the first of them are made.
            if report.changes.visible() {
                announce(added, removed);
                shared.broadcast(sources_event(shared));
            }
            // A whole library listed, or many thumbnails to make (a new
            // folder's, which take a while each, or what a round that gave
            // up left): windows hear how it goes and see the photos as they
            // come, every four seconds.
            let telling = report.listed || missing > 100;
            crate::sync::make_thumbs(source.as_ref(), &mut db, &library, 16, &|done, total| {
                if telling {
                    building.step("thumbs", done as u64, total as u64);
                }
            }, &stop)?;
        }
        eprintln!("pinholed: sync: library version {version}: {added} new, {removed} removed, {missing} thumbnails wanted");
        announce(added, removed);
        shared.broadcast(sources_event(shared));
        Ok(())
    })();
    building.finish();
    match result {
        // Another source is shown: what was made stays, and the rest is
        // made when this one is shown again.
        Err(e) if e.downcast_ref::<crate::source::Stopped>().is_some() => {
            eprintln!("pinholed: sync: {} given up for now: another source is shown", source.id());
            Ok(())
        }
        other => other,
    }
}
