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
}

fn health_event(health: Health) -> Value {
    json!({"event": "health", "state": health.name()})
}

/// What a window says about the source: its name, and whether it is one
/// that needs signing in.
fn source_info(source: &dyn Source) -> Value {
    let mut info = json!({"kind": source.kind(), "name": source.name(), "needs_sign_in": source.needs_sign_in()});
    if let Some(path) = source.id().strip_prefix("folder:") {
        info["path"] = json!(path);
    }
    info
}

fn source_event(source: &dyn Source) -> Value {
    let mut event = source_info(source);
    event["event"] = json!("source");
    event
}

/// Photos from another source from now on: kept in the settings, the
/// library switched to that source's own (made from scratch if it has none
/// yet), and every window told to read it.
fn set_source(shared: &Arc<Shared>, params: &Value) -> Result<Value> {
    let source = crate::sync::source_for(Some(params), shared.config.auth.clone())?;
    if let Some(path) = source.id().strip_prefix("folder:") {
        if !Path::new(path).is_dir() {
            return Err(anyhow!("{path} is not a folder"));
        }
    }
    if source.id() == shared.source().id() {
        return Ok(source_info(&*source));
    }
    {
        // Not while a sync round writes the library.
        let _writing = shared.storage.library_lock.lock().unwrap();
        crate::storage::update_settings(&shared.config.settings, |settings| settings["source"] = params.clone())?;
        *shared.source.write().unwrap() = source.clone();
        *shared.health.lock().unwrap() = Health::Fine;
        eprintln!("pinholed: photos from {} now", source.name());
        shared.broadcast(source_event(&*source));
        shared.broadcast(health_event(Health::Fine));
        shared.storage.switch_library(crate::storage::library_for(&shared.storage.location(), &*source));
    }
    let _ = shared.sync_now.lock().unwrap().send(());
    if let Err(e) = source.start() {
        shared.failed(&e);
    }
    Ok(source_info(&*source))
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
    // An index from an older Pinhole is brought up to date before anything
    // reads it.
    let index = config.library.join("library.sqlite");
    if index.exists() {
        crate::db::open(&index).context("opening the library")?;
    }
    eprintln!("pinholed: photos from {}", config.source.name());
    let listener = bind(&config.socket)?;
    eprintln!("pinholed: listening on {}", config.socket.display());

    let (lookup_tx, lookup_rx) = mpsc::channel::<Job>();
    let (sync_tx, sync_rx) = mpsc::channel::<()>();
    let (download_tx, download_rx) = mpsc::channel::<Job>();
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
    let health = *shared.health.lock().unwrap();
    if health != Health::Fine {
        let _ = writeln!(out.lock().unwrap(), "{}", health_event(health));
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
            }))),
            Some("set_source") => match set_source(&shared, &request["params"]) {
                Ok(info) => reply.send(Ok(info)),
                Err(e) => reply.send(Err(("bad_request", format!("{e:#}")))),
            },
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

fn sync_round(shared: &Shared) -> Result<()> {
    let source = shared.source();
    // Not while the library moves.
    let _writing = shared.storage.library_lock.lock().unwrap();
    let library = shared.storage.library();
    std::fs::create_dir_all(&library)?;
    let mut db = crate::db::open(&library.join("library.sqlite"))?;
    crate::sync::claim(source.as_ref(), &mut db, &library)?;
    let report = crate::sync::catch_up(source.as_ref(), &mut db)?;
    let (added, removed) = (report.changes.added.len(), report.changes.removed.len());
    if !report.changes.visible() {
        return Ok(());
    }
    if added > 0 {
        crate::sync::make_thumbs(source.as_ref(), &mut db, &library, 16, &|_, _| {})?;
    }
    let version: i64 = crate::db::meta(&db, "library_version").and_then(|v| v.parse().ok()).unwrap_or(0);
    eprintln!("pinholed: sync: library version {version}: {added} new, {removed} removed");
    shared.broadcast(json!({"event": "library_changed", "version": version, "added": added, "removed": removed}));
    Ok(())
}
