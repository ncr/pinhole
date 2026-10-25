//! pinholed: hands the window local files for any rendition of a photo,
//! from its cache or from iCloud. See docs/service.md.

use std::collections::HashMap;
use std::io::{BufRead, BufReader, Write};
use std::os::unix::fs::PermissionsExt;
use std::os::unix::net::{UnixListener, UnixStream};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{mpsc, Arc, Mutex};
use std::time::{Duration, SystemTime};

use anyhow::{anyhow, Context, Result};
use rusqlite::{params, Connection, OpenFlags};
use serde_json::{json, Value};

use crate::auth::{ProcessProvider, ProviderError};
use crate::cloudkit::{Downloader, PhotosClient};
use crate::source::pair_records_with_leftovers;
use crate::sync::{now_secs, thumb_path};

/// A link younger than this is used as is; older ones are looked up again.
const URL_FRESH_SECS: i64 = 20 * 60;
/// How long the lookup thread waits for more requests to batch with the
/// first, and how many assets one lookup may carry.
const BATCH_WINDOW: Duration = Duration::from_millis(25);
const BATCH_MAX: usize = 100;

pub struct Config {
    pub library: PathBuf,
    pub cache: PathBuf,
    pub socket: PathBuf,
    pub auth: PathBuf,
    pub cache_cap: u64,
    pub downloads: usize,
    /// Seconds between rounds of following iCloud's change feed.
    pub sync_every: u64,
}

/// Where to answer one request.
#[derive(Clone)]
struct Reply {
    id: Value,
    out: Arc<Mutex<UnixStream>>,
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
    asset: String,
    master: String,
    rendition: String,
    path: PathBuf,
    url: Option<String>,
    retried: bool,
}

struct Shared {
    config: Config,
    lookups: Mutex<mpsc::Sender<Job>>,
    downloads: Mutex<mpsc::Sender<Job>>,
    /// Requests waiting on each file being fetched: a second request for the
    /// same file joins the first instead of downloading it again.
    waiting: Mutex<HashMap<PathBuf, (Value, Vec<Reply>)>>,
    in_flight: AtomicUsize,
    downloader: Mutex<Option<Downloader>>,
    /// Every connected window, to tell when the library changed.
    listeners: Mutex<Vec<Arc<Mutex<UnixStream>>>>,
    /// Wakes the sync thread before its next round is due.
    sync_now: Mutex<mpsc::Sender<()>>,
}

impl Shared {
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
    fn finish(&self, path: &Path, result: std::result::Result<(), (&str, String)>) {
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

fn renditions(kind: &str) -> Option<&'static [&'static str]> {
    Some(match kind {
        "medium" => &["JPEGMed", "JPEGFull", "JPEGLarge"],
        "original" => &["Original"],
        "video" => &["VidMed", "VidFull", "Original"],
        "live" => &["OriginalVidCompl"],
        _ => return None,
    })
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
    let listener = bind(&config.socket)?;
    eprintln!("pinholed: listening on {}", config.socket.display());

    let (lookup_tx, lookup_rx) = mpsc::channel::<Job>();
    let (sync_tx, sync_rx) = mpsc::channel::<()>();
    let (download_tx, download_rx) = mpsc::channel::<Job>();
    let shared = Arc::new(Shared {
        config,
        lookups: Mutex::new(lookup_tx),
        downloads: Mutex::new(download_tx),
        waiting: Mutex::new(HashMap::new()),
        in_flight: AtomicUsize::new(0),
        downloader: Mutex::new(None),
        listeners: Mutex::new(Vec::new()),
        sync_now: Mutex::new(sync_tx),
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
        let shared = shared.clone();
        std::thread::spawn(move || evict_loop(shared));
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
    let db = Connection::open_with_flags(
        shared.config.library.join("library.sqlite"),
        OpenFlags::SQLITE_OPEN_READ_ONLY | OpenFlags::SQLITE_OPEN_NO_MUTEX,
    )?;
    for line in BufReader::new(stream).lines() {
        let line = line?;
        if line.trim().is_empty() {
            continue;
        }
        let request: Value = match serde_json::from_str(&line) {
            Ok(v) => v,
            Err(_) => {
                Reply { id: Value::Null, out: out.clone() }.send(Err(("bad_request", "not JSON".into())));
                continue;
            }
        };
        let reply = Reply { id: request["id"].clone(), out: out.clone() };
        match request["method"].as_str() {
            Some("ping") => reply.send(Ok(json!({}))),
            Some("sync") => {
                let _ = shared.sync_now.lock().unwrap().send(());
                reply.send(Ok(json!({})))
            }
            Some("status") => reply.send(Ok(json!({
                "library": shared.config.library,
                "cache": shared.config.cache,
                "cache_bytes": cache_size(&shared.config.cache),
                "in_flight": shared.in_flight.load(Ordering::Relaxed),
            }))),
            Some("fetch") => fetch(&shared, &db, &request["params"], reply),
            other => reply.send(Err(("bad_request", format!("unknown method {other:?}")))),
        }
    }
    Ok(())
}

fn fetch(shared: &Shared, db: &Connection, params: &Value, reply: Reply) {
    let (Some(asset), Some(kind)) = (params["asset"].as_str(), params["kind"].as_str()) else {
        return reply.send(Err(("bad_request", "fetch needs asset and kind".into())));
    };
    let Some(candidates) = renditions(kind) else {
        return reply.send(Err(("bad_request", format!("unknown kind {kind}"))));
    };

    // The first of the candidate renditions this asset has.
    let mut found = None;
    for rendition in candidates {
        let row = db.query_row(
            "SELECT a.master_id, r.checksum, r.file_type, r.url, r.url_fetched_at FROM assets a
             JOIN resources r ON r.asset_id = a.id AND r.kind = ?2 WHERE a.id = ?1",
            params![asset, rendition],
            |r| {
                Ok((r.get::<_, String>(0)?, r.get::<_, Option<String>>(1)?, r.get::<_, Option<String>>(2)?,
                    r.get::<_, Option<String>>(3)?, r.get::<_, Option<i64>>(4)?))
            },
        );
        if let Ok(row) = row {
            found = Some((*rendition, row));
            break;
        }
    }
    let Some((rendition, (master, checksum, file_type, url, fetched))) = found else {
        return reply.send(Err(("not_found", format!("{asset} has no {kind} rendition"))));
    };
    let file_type = file_type.unwrap_or_default();
    let name = checksum.unwrap_or_else(|| format!("{asset}-{rendition}"));
    let path = shared.config.cache.join(kind).join(
        thumb_path(&name).replacen("thumbs/", "", 1).replace(".jpg", &format!(".{}", extension(&file_type))),
    );
    let info = json!({"path": path, "type": file_type, "rendition": rendition, "cached": true});

    if path.exists() {
        touch(&path);
        return reply.send(Ok(info));
    }

    let mut waiting = shared.waiting.lock().unwrap();
    if let Some((_, replies)) = waiting.get_mut(&path) {
        replies.push(reply);
        return;
    }
    let mut info = info;
    info["cached"] = json!(false);
    waiting.insert(path.clone(), (info, vec![reply]));
    shared.in_flight.fetch_add(1, Ordering::Relaxed);
    drop(waiting);

    let fresh = fetched.is_some_and(|t| now_secs() - t < URL_FRESH_SECS);
    let job = Job {
        asset: asset.to_owned(),
        master,
        rendition: rendition.to_owned(),
        path,
        url: if fresh { url } else { None },
        retried: false,
    };
    let _ = shared.lookups.lock().unwrap().send(job);
}

/// Owns the iCloud client. Gathers requests for a moment, looks up fresh
/// links for all that need one in one go, and passes them to the downloads.
fn lookup_loop(shared: Arc<Shared>, rx: mpsc::Receiver<Job>) {
    // Signed in at start: the provider takes a few seconds to check the
    // session, which the first request should not have to wait for.
    let mut client: Option<PhotosClient> = match ProcessProvider::spawn(shared.config.auth.clone(), None)
        .and_then(|p| PhotosClient::new(Box::new(p)))
    {
        Ok(c) => {
            *shared.downloader.lock().unwrap() = Some(c.downloader());
            eprintln!("pinholed: signed in as {}", c.session().apple_id);
            Some(c)
        }
        Err(e) => {
            eprintln!("pinholed: not signed in yet: {e:#}");
            None
        }
    };
    let mut db: Option<Connection> = None;
    while let Ok(first) = rx.recv() {
        std::thread::sleep(BATCH_WINDOW);
        let mut batch = vec![first];
        while batch.len() < BATCH_MAX {
            match rx.try_recv() {
                Ok(job) => batch.push(job),
                Err(_) => break,
            }
        }

        if client.is_none() {
            match ProcessProvider::spawn(shared.config.auth.clone(), None)
                .and_then(|p| PhotosClient::new(Box::new(p)))
            {
                Ok(c) => {
                    *shared.downloader.lock().unwrap() = Some(c.downloader());
                    client = Some(c);
                }
                Err(e) => {
                    let code = if e.downcast_ref::<ProviderError>().is_some() { "needs_sign_in" } else { "unavailable" };
                    for job in batch {
                        shared.finish(&job.path, Err((code, format!("{e:#}"))));
                    }
                    continue;
                }
            }
        }
        let client = client.as_mut().expect("client");

        let (ready, stale): (Vec<Job>, Vec<Job>) = batch.into_iter().partition(|j| j.url.is_some());
        for job in ready {
            let _ = shared.downloads.lock().unwrap().send(job);
        }
        if stale.is_empty() {
            continue;
        }
        let names: Vec<&str> = stale.iter().flat_map(|j| [j.asset.as_str(), j.master.as_str()]).collect();
        let records = match client.lookup(&names) {
            Ok(r) => r["records"].as_array().cloned().unwrap_or_default(),
            Err(e) => {
                for job in stale {
                    shared.finish(&job.path, Err(("unavailable", format!("looking up links: {e:#}"))));
                }
                continue;
            }
        };
        let (assets, _) = pair_records_with_leftovers(&records, false);
        let by_id: HashMap<&str, &crate::source::Asset> = assets.iter().map(|a| (a.id.as_str(), a)).collect();

        // Fresh links go back into the index, so the next request for any
        // rendition of these assets needs no lookup.
        if db.is_none() {
            db = db_for_writing(&shared.config.library);
        }
        let now = now_secs();
        for mut job in stale {
            let url = by_id.get(job.asset.as_str()).and_then(|a| a.resource(&job.rendition)).and_then(|r| r.url.clone());
            let Some(url) = url else {
                shared.finish(&job.path, Err(("not_found", format!("iCloud has no {} for {}", job.rendition, job.asset))));
                continue;
            };
            if let Some(db) = &db {
                if let Some(asset) = by_id.get(job.asset.as_str()) {
                    for r in &asset.resources {
                        let _ = db.execute(
                            "UPDATE resources SET url = ?3, url_fetched_at = ?4 WHERE asset_id = ?1 AND kind = ?2",
                            params![job.asset, r.kind, r.url, now],
                        );
                    }
                }
            }
            job.url = Some(url);
            let _ = shared.downloads.lock().unwrap().send(job);
        }
    }
}

fn db_for_writing(library: &Path) -> Option<Connection> {
    let db = Connection::open(library.join("library.sqlite")).ok()?;
    let _ = db.busy_timeout(Duration::from_secs(5));
    Some(db)
}

fn download_loop(shared: Arc<Shared>, rx: Arc<Mutex<mpsc::Receiver<Job>>>) {
    loop {
        let job = { rx.lock().unwrap().recv() };
        let Ok(job) = job else { return };
        let downloader = shared.downloader.lock().unwrap().clone();
        let Some(downloader) = downloader else {
            shared.finish(&job.path, Err(("unavailable", "no iCloud session".into())));
            continue;
        };
        match download(&downloader, &job) {
            Ok(()) => shared.finish(&job.path, Ok(())),
            // An expired link: look it up again, once.
            Err(Fetch::Expired) if !job.retried => {
                let mut again = job.clone();
                again.url = None;
                again.retried = true;
                let _ = shared.lookups.lock().unwrap().send(again);
            }
            Err(Fetch::Expired) => shared.finish(&job.path, Err(("unavailable", "the download link kept expiring".into()))),
            Err(Fetch::Failed(e)) => shared.finish(&job.path, Err(("unavailable", e))),
        }
    }
}

enum Fetch {
    Expired,
    Failed(String),
}

fn download(downloader: &Downloader, job: &Job) -> std::result::Result<(), Fetch> {
    let url = job.url.as_deref().ok_or_else(|| Fetch::Failed("no link".into()))?;
    let mut response = downloader.fetch(url, None, false).map_err(|e| Fetch::Failed(e.to_string()))?;
    let status = response.status();
    if matches!(status.as_u16(), 401 | 403 | 404 | 410) {
        return Err(Fetch::Expired);
    }
    if !status.is_success() {
        return Err(Fetch::Failed(format!("HTTP {status}")));
    }
    if let Some(dir) = job.path.parent() {
        std::fs::create_dir_all(dir).map_err(|e| Fetch::Failed(e.to_string()))?;
    }
    // Written beside the target and renamed: a reader never sees half a file.
    let part = job.path.with_extension("part");
    let result = (|| -> std::io::Result<()> {
        let mut file = std::fs::File::create(&part)?;
        std::io::copy(&mut response, &mut file)?;
        file.sync_data()?;
        std::fs::rename(&part, &job.path)
    })();
    if let Err(e) = result {
        let _ = std::fs::remove_file(&part);
        return Err(Fetch::Failed(e.to_string()));
    }
    Ok(())
}

fn cache_files(dir: &Path, out: &mut Vec<(SystemTime, u64, PathBuf)>) {
    let Ok(entries) = std::fs::read_dir(dir) else { return };
    for entry in entries.flatten() {
        let path = entry.path();
        let Ok(meta) = entry.metadata() else { continue };
        if meta.is_dir() {
            cache_files(&path, out);
        } else if path.extension().is_none_or(|e| e != "part") {
            out.push((meta.modified().unwrap_or(SystemTime::UNIX_EPOCH), meta.len(), path));
        }
    }
}

fn cache_size(dir: &Path) -> u64 {
    let mut files = Vec::new();
    cache_files(dir, &mut files);
    files.iter().map(|f| f.1).sum()
}

/// Every two minutes: past the size limit, the least recently used files go
/// until the cache is back under nine tenths of it.
fn evict_loop(shared: Arc<Shared>) {
    loop {
        let mut files = Vec::new();
        cache_files(&shared.config.cache, &mut files);
        let mut total: u64 = files.iter().map(|f| f.1).sum();
        if total > shared.config.cache_cap {
            files.sort();
            for (_, size, path) in files {
                if total <= shared.config.cache_cap / 10 * 9 {
                    break;
                }
                if std::fs::remove_file(&path).is_ok() {
                    total -= size;
                }
            }
        }
        std::thread::sleep(Duration::from_secs(120));
    }
}

/// Follows iCloud's change feed: every `sync_every` seconds, or when a
/// window asks, it applies what changed, fetches thumbnails for anything new,
/// and tells every window that the library changed (only when something it
/// draws did). A change token iCloud no longer accepts means a full walk.
fn sync_loop(shared: Arc<Shared>, wake: mpsc::Receiver<()>) {
    let mut client: Option<PhotosClient> = None;
    loop {
        if client.is_none() {
            match ProcessProvider::spawn(shared.config.auth.clone(), None).and_then(|p| PhotosClient::new(Box::new(p))) {
                Ok(c) => client = Some(c),
                Err(e) => eprintln!("pinholed: sync: not signed in: {e:#}"),
            }
        }
        if let Some(c) = client.as_mut() {
            if let Err(e) = sync_round(&shared, c) {
                eprintln!("pinholed: sync: {e:#}");
            }
        }
        // Until the next round, or a window asking for one now.
        match wake.recv_timeout(Duration::from_secs(shared.config.sync_every.max(5))) {
            Ok(()) | Err(mpsc::RecvTimeoutError::Timeout) => {}
            Err(mpsc::RecvTimeoutError::Disconnected) => return,
        }
        // Several asks at once make one round.
        while wake.try_recv().is_ok() {}
    }
}

fn sync_round(shared: &Shared, client: &mut PhotosClient) -> Result<()> {
    let mut db = Connection::open(shared.config.library.join("library.sqlite"))?;
    db.busy_timeout(Duration::from_secs(10))?;
    let (added, removed, visible) = match crate::sync::apply_changes(client, &mut db, false) {
        Ok(r) => (r.added.len(), r.removed.len(), r.visible()),
        Err(e) if e.downcast_ref::<crate::sync::TokenExpired>().is_some() => {
            eprintln!("pinholed: sync: {e}; walking the whole library");
            let r = crate::sync::walk(client, &mut db, 100, 4, false, &|_, _| {})?;
            (r.assets as usize, r.removed, true)
        }
        Err(e) => return Err(e),
    };
    if !visible {
        return Ok(());
    }
    if added > 0 {
        crate::sync::download_thumbs(client, &mut db, &shared.config.library, 16, &|_, _| {})?;
    }
    let version: i64 = crate::db::meta(&db, "library_version").and_then(|v| v.parse().ok()).unwrap_or(0);
    eprintln!("pinholed: sync: library version {version}: {added} new, {removed} removed");
    shared.broadcast(json!({"event": "library_changed", "version": version, "added": added, "removed": removed}));
    Ok(())
}
