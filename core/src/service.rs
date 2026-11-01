//! pinholed: hands the window local files for any rendition of a photo,
//! from its cache or from the source, and keeps the library in step with
//! the source. It talks to the source only through the Source trait. See
//! docs/service.md and docs/sources.md.

use std::collections::HashMap;
use std::io::{BufRead, BufReader, Read, Write};
use std::os::unix::fs::PermissionsExt;
use std::os::unix::net::{UnixListener, UnixStream};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::{mpsc, Arc, Mutex, RwLock};
use std::time::{Duration, SystemTime};

use anyhow::{anyhow, Context, Result};
use rusqlite::{params, Connection, OpenFlags};
use serde_json::{json, Value};

use crate::kinds::{Kinds, SignedIn};
use crate::source::{FetchError, Health, Heard, Source, Stored, Ticket, Unsupported, Want, Watch};
use crate::storage::Storage;
use crate::stream::Streams;
use crate::sync::thumb_path;

/// How long the lookup thread waits for more requests to batch with the
/// first, and how many it hands the source at once.
const BATCH_WINDOW: Duration = Duration::from_millis(25);
const BATCH_MAX: usize = 100;

/// The window's and the service's protocol (docs/service.md): raised when
/// either side would misread what the other says.
pub const PROTOCOL: u32 = 1;

/// How long a line to a window may take to write: one that has not read for
/// this long is hung up on.
const WRITE_WITHIN: Duration = Duration::from_secs(5);

/// How many lines may wait for a window: one that falls further behind is
/// hung up on.
const QUEUE_MAX: usize = 4096;

/// How long a service of a library given waits, once its last window has
/// gone, for its downloads to end.
const FINISH_WITHIN: Duration = Duration::from_secs(30);

pub struct Config {
    /// The folder everything is kept in (storage.rs), and in it, unless
    /// given otherwise, the library and the cache.
    pub location: PathBuf,
    pub library: PathBuf,
    pub cache: PathBuf,
    pub socket: PathBuf,
    /// Where the photos come from.
    pub source: Arc<dyn Source>,
    /// Every kind of source there is, and which are on (kinds.rs).
    pub kinds: Kinds,
    /// The limit for all of it, if one was set.
    pub limit: Option<u64>,
    /// Where the location and limit are kept when the window changes them.
    pub settings: PathBuf,
    /// The plugins' switches (plugins.rs), which the kinds follow: only
    /// read.
    pub plugins: PathBuf,
    /// A folder of photos from before (the old cache) to move in.
    pub adopt: Option<PathBuf>,
    pub downloads: usize,
    /// Seconds between rounds of following the source's changes, if not
    /// as often as the source would.
    pub sync_every: Option<u64>,
    /// A library given to a window (`pinhole --library`) other than the
    /// shared service's, served to it alone: only read and fetched for.
    /// Never synced (the source neither followed nor watched, the settings
    /// not followed), nothing of the cache evicted (the shared service
    /// does), no move resumed and nothing adopted, nothing changed for a
    /// window (the source, the storage, favourites, files added); and the
    /// service ends once no window has been connected for this long.
    /// None: the shared service.
    pub given: Option<Duration>,
}

/// What a window may not ask of a service of a library given (Config::given).
const CHANGES: [&str; 9] =
    ["set_source", "remove_source", "sign_out", "sign_in", "submit_code", "add", "set_favorite", "set_storage", "plugins_changed"];

/// Where to answer one request.
#[derive(Clone)]
struct Reply {
    id: Value,
    out: Out,
    /// Answer as soon as the download starts, with a URL that plays it.
    stream: bool,
}

/// A window, or the service itself, waiting on a file it makes from this
/// one (an effect from the motion).
#[derive(Clone)]
enum Out {
    Window(Arc<Window>),
    Service(mpsc::Sender<std::result::Result<Value, (String, String)>>),
}

/// A connected window's way out. Lines join its queue and a thread of its
/// own writes them, so whoever has news or an answer for it never waits on
/// how fast it reads: not a download, not another window.
struct Window {
    queue: mpsc::SyncSender<String>,
    /// To hang up on it.
    stream: UnixStream,
}

impl Window {
    fn open(stream: &UnixStream) -> Result<Arc<Window>> {
        // A window that stops reading is hung up on, not waited for.
        stream.set_write_timeout(Some(WRITE_WITHIN))?;
        let (queue, lines) = mpsc::sync_channel::<String>(QUEUE_MAX);
        let mut out = stream.try_clone()?;
        std::thread::spawn(move || {
            for line in lines {
                if out.write_all(line.as_bytes()).and_then(|_| out.flush()).is_err() {
                    // Cut off, maybe halfway through a line: it starts
                    // afresh with a connection of its own.
                    let _ = out.shutdown(std::net::Shutdown::Both);
                    return;
                }
            }
        });
        Ok(Arc::new(Window { queue, stream: stream.try_clone()? }))
    }

    /// Queue a line; false once the window has gone, or fallen so far
    /// behind that it is hung up on.
    fn send(&self, line: impl std::fmt::Display) -> bool {
        match self.queue.try_send(format!("{line}\n")) {
            Ok(()) => true,
            Err(_) => {
                let _ = self.stream.shutdown(std::net::Shutdown::Both);
                false
            }
        }
    }
}

impl Reply {
    /// A line of news for this window alone, before the answer.
    fn event(&self, event: Value) {
        let Out::Window(out) = &self.out else { return };
        out.send(event);
    }

    fn send(&self, result: std::result::Result<Value, (&str, String)>) {
        let out = match &self.out {
            Out::Window(out) => out,
            Out::Service(tx) => {
                let _ = tx.send(result.map_err(|(code, message)| (code.to_owned(), message)));
                return;
            }
        };
        let line = match result {
            Ok(r) => json!({"id": self.id, "ok": true, "result": r}),
            Err((code, message)) => json!({"id": self.id, "ok": false, "error": {"code": code, "message": message}}),
        };
        out.send(line);
    }
}

/// One file to bring into the cache: from the source, and by the
/// library, it was asked of, whichever is shown by the time it is got.
#[derive(Clone)]
struct Job {
    source: Arc<dyn Source>,
    /// Its library, inside the folder everything is kept in (absolute if
    /// it is elsewhere): found there wherever that folder has moved since.
    library: PathBuf,
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
    listeners: Mutex<Vec<Arc<Window>>>,
    /// Wakes the sync thread before its next round is due.
    sync_now: Mutex<mpsc::Sender<()>>,
    streams: Arc<Streams>,
    /// The play URL of each video downloading now, by its cache path.
    playable: Mutex<HashMap<PathBuf, String>>,
    /// Whether the source can be reached (and the session is good).
    health: Mutex<Health>,
    /// A library being built, as the last `progress` event said.
    progress: Mutex<Option<Value>>,
    /// plugins.json as the kinds were last set from it (`plugins::stamp`):
    /// read again when it changes.
    plugins_seen: Mutex<Option<(SystemTime, u64)>>,
    /// Bumped as another source is shown, before its library is taken: a
    /// sync round of the one before sees it and gives up, so the switch
    /// does not wait for a library being built.
    shown: std::sync::atomic::AtomicU64,
    /// Held while the source shown changes (set_source, remove_source, a
    /// sign-in, a plugin turned on or off), so the settings and the source
    /// shown agree.
    changing: Mutex<()>,
    /// set_source and remove_source, done one after another in the order
    /// asked (keys pressed in a row: the last is the one shown).
    changes: Mutex<mpsc::Sender<(String, Value, Reply)>>,
    /// `shown` as the last sync round began: a round that sees it changed
    /// is the first since a source was shown, and finishes what an earlier
    /// round of it gave up (thumbnails still missing).
    rounds_shown: std::sync::atomic::AtomicU64,
    /// When sync rounds ran and run next: the `rounds` event.
    rounds: Mutex<Rounds>,
    /// The source shown, watched for changes as they happen, if it can be.
    watch: Mutex<Option<Watch>>,
    /// Held while files are added: one asset at a time, whichever window
    /// dropped them.
    adding: Mutex<()>,
    /// set_favorite, done one after another in the order asked: F pressed
    /// twice is a favourite and back, never the other way round.
    favorites: Mutex<mpsc::Sender<(Value, Reply)>>,
    /// How many connections there are, and since when there have been
    /// none: a service of a library given ends after a while of none.
    connections: Mutex<(usize, std::time::Instant)>,
}

/// When each source was last looked at for changes (the end of its last
/// round that worked, by its id; since this service started), when the
/// next round of the one shown is due (none while one runs), and whether
/// it tells of changes itself as they happen.
#[derive(Default)]
struct Rounds {
    last: HashMap<String, SystemTime>,
    next: Option<SystemTime>,
    push: bool,
}

fn millis(time: SystemTime) -> u64 {
    time.duration_since(SystemTime::UNIX_EPOCH).map_or(0, |d| d.as_millis() as u64)
}

/// The `rounds` event for the source shown (docs/service.md); with none,
/// `source` is null.
fn rounds_event(source: &dyn Source, rounds: &Rounds) -> Value {
    if source.kind() == "none" {
        return json!({"event": "rounds", "source": null, "last": null, "next": null, "push": false});
    }
    let id = source.id();
    json!({
        "event": "rounds",
        "source": id,
        "last": rounds.last.get(&id).copied().map(millis),
        "next": rounds.next.map(millis),
        "push": rounds.push,
    })
}

fn health_event(health: Health) -> Value {
    json!({"event": "health", "state": health.name()})
}

/// What a window says about the source: its kind and the plugin it is
/// of, its name, which of its kind it is (`details`: a folder's `path`,
/// pinhole.icloud's `account`), and whether it is one that needs signing
/// in. With none, `kind` is null.
fn source_info(kinds: &Kinds, source: &dyn Source) -> Value {
    if source.kind() == "none" {
        return json!({"kind": null, "plugin": null, "name": "", "needs_sign_in": false, "accepts": []});
    }
    let plugin = kinds.of(source.kind()).map(|k| k.plugin().to_owned());
    let mut info = json!({"kind": source.kind(), "plugin": plugin, "id": source.id(), "name": source.name(),
                          "needs_sign_in": source.needs_sign_in(), "accepts": source.accepts()});
    if let (Some(info), Some(details)) = (info.as_object_mut(), source.details().as_object()) {
        info.extend(details.clone());
    }
    info
}

fn source_event(kinds: &Kinds, source: &dyn Source) -> Value {
    let mut event = source_info(kinds, source);
    event["event"] = json!("source");
    event
}

/// Which kinds of source there are, the plugins they are of, and which
/// are on (docs/service.md, `kinds`).
fn kinds_event(kinds: &Kinds) -> Value {
    let mut event = kinds.report();
    event["event"] = json!("kinds");
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

/// Every source added whose kind is on, in order, with what is in its
/// library, and the one shown (docs/service.md, `sources`).
fn sources_report(shared: &Shared) -> Value {
    let kinds = &shared.config.kinds;
    let settings = shared.settings();
    let location = shared.storage.location();
    let current = shared.source().id();
    let sources: Vec<Value> = settings
        .sources
        .iter()
        .filter_map(|entry| {
            let kind = kinds.of_entry(entry).filter(|k| kinds.on(k.kind()))?;
            let source = kind.source(entry).ok()?;
            let id = source.id();
            let library = if id == current { shared.storage.library() } else { crate::storage::library_for(&location, &*source) };
            let (count, thumbs) = library_counts(&library);
            let mut info = json!({"id": id, "kind": kind.kind(), "plugin": kind.plugin(), "count": count, "thumbs": thumbs});
            if let (Some(info), Some(described)) = (info.as_object_mut(), kind.describe(entry).as_object()) {
                info.extend(described.clone());
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

/// A request's error: its code and why.
type Refused = (String, String);

fn bad(message: impl Into<String>) -> Refused {
    ("bad_request".to_owned(), message.into())
}

/// The kind of that name, if it is on: one that is off is `turned_off`,
/// one there is none of a bad request.
fn kind_on(shared: &Shared, kind: &str) -> std::result::Result<Arc<dyn crate::kinds::Kind>, Refused> {
    let kinds = &shared.config.kinds;
    let found = kinds.of(kind).ok_or_else(|| bad(format!("unknown source {kind:?}")))?;
    if !kinds.on(kind) {
        return Err(("turned_off".to_owned(), format!("{kind} sources are turned off: {} is off", found.plugin())));
    }
    Ok(found)
}

/// A source added before, by its id, with its kind, which must be on.
fn added(shared: &Shared, id: &str) -> std::result::Result<(Value, Arc<dyn crate::kinds::Kind>), Refused> {
    let entry = crate::storage::source_entry(&shared.settings(), &shared.config.kinds, id).ok_or_else(|| bad(format!("no source {id}")))?;
    let kind = kind_on(shared, entry["kind"].as_str().unwrap_or(""))?;
    Ok((entry, kind))
}

/// Show another source: one added before, by its `id`, or a new one by
/// `kind` and what that kind needs (a folder's path, pinhole.icloud's
/// account signed in to), added first. Kept in the settings, the library switched to that
/// source's own (made from scratch if it has none yet), and every window
/// told to read it.
fn set_source(shared: &Arc<Shared>, params: &Value) -> std::result::Result<Value, Refused> {
    let kinds = &shared.config.kinds;
    let entry = match params["id"].as_str() {
        Some(id) => added(shared, id)?.0,
        None => {
            let kind = params["kind"].as_str().ok_or_else(|| bad("set_source needs an id or a kind"))?;
            kind_on(shared, kind)?.entry_for(params)?
        }
    };
    let source = kinds.source_for(Some(&entry)).map_err(|e| bad(format!("{e:#}")))?;
    let _changing = shared.changing.lock().unwrap();
    crate::storage::show_source(&shared.config.settings, kinds, Some(&entry)).map_err(|e| bad(format!("{e:#}")))?;
    if source.id() != shared.source().id() {
        switch(shared, source.clone()).map_err(|e| bad(format!("{e:#}")))?;
    }
    shared.broadcast(sources_event(shared));
    Ok(source_info(kinds, &*source))
}

/// Take a source out: its library deleted (never the photos themselves),
/// and whatever its kind kept for it apart (pinhole.icloud's session); if it
/// was the one shown, the first left whose kind is on is shown instead, or
/// none.
fn remove_source(shared: &Arc<Shared>, params: &Value) -> std::result::Result<Value, Refused> {
    let kinds = &shared.config.kinds;
    let id = params["id"].as_str().ok_or_else(|| bad("remove_source needs an id"))?;
    let _changing = shared.changing.lock().unwrap();
    let (entry, kind) = added(shared, id)?;
    let source = kind.source(&entry).map_err(|e| bad(format!("{e:#}")))?;
    crate::storage::remove_source(&shared.config.settings, kinds, id).map_err(|e| bad(format!("{e:#}")))?;
    if shared.source().id() == id {
        let next = kinds.source_for(kinds.shown(&shared.settings()).as_ref()).map_err(|e| bad(format!("{e:#}")))?;
        switch(shared, next).map_err(|e| bad(format!("{e:#}")))?;
    }
    kind.removed(&entry);
    // Only ever a folder under library/ that holds this source's photos,
    // never the library shown now.
    let library = crate::storage::library_for(&shared.storage.location(), &*source);
    let libraries = shared.storage.location().join("library");
    if library.starts_with(&libraries)
        && library != libraries
        && library != shared.storage.library()
        && crate::storage::library_owner(&library).as_deref() == Some(id)
    {
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

/// Sign out of a source (pinhole.icloud's): its session deleted, nothing else.
/// It stays among the sources with its library (and its owner's name),
/// which shows what is on this computer until it is signed in to again, at
/// once.
fn sign_out(shared: &Arc<Shared>, params: &Value) -> std::result::Result<Value, Refused> {
    let id = params["id"].as_str().ok_or_else(|| bad("sign_out needs an id"))?;
    let _changing = shared.changing.lock().unwrap();
    let (entry, kind) = added(shared, id)?;
    kind.sign_out(&entry)?;
    // Shown: a fresh source, which has no session to go on.
    if shared.source().id() == id {
        let source = kind.source(&entry).map_err(|e| bad(format!("{e:#}")))?;
        switch(shared, source).map_err(|e| bad(format!("{e:#}")))?;
    }
    shared.broadcast(sources_event(shared));
    Ok(sources_report(shared))
}

/// What could be added (docs/service.md, `discover`): each kind that is
/// on says, under `kinds`, and what each says is at the top as well.
fn discover(shared: &Shared) -> Value {
    let added = shared.settings().sources;
    let mut found = json!({"kinds": {}});
    for kind in shared.config.kinds.all().iter().filter(|k| shared.config.kinds.on(k.kind())) {
        let said = kind.discover(&added);
        // At the top only the built-in kinds' fields, which windows from
        // before kinds read: a provider's never take their names.
        if let Some(fields) = said.as_object().filter(|_| !shared.config.kinds.provided(kind.kind())) {
            for (key, value) in fields {
                found[key.as_str()] = value.clone();
            }
        }
        found["kinds"][kind.kind()] = said;
    }
    found
}

/// Sign in to a source of `kind`: signed in, it is added and shown. None
/// said is the first kind that signs in (iCloud), as older windows and
/// `pinhole sign-in` ask.
fn sign_in(shared: &Arc<Shared>, method: &str, params: &Value) -> std::result::Result<Value, Refused> {
    let kind = match params["kind"].as_str() {
        Some(kind) => kind.to_owned(),
        None => shared.config.kinds.all().iter().find(|k| k.needs_sign_in()).map(|k| k.kind().to_owned()).unwrap_or_default(),
    };
    let kind = kind_on(shared, &kind)?;
    let signed = if method == "submit_code" { kind.submit_code(params)? } else { kind.sign_in(params)? };
    match signed {
        SignedIn::NeedsCode => Ok(json!({"state": "needs_code"})),
        SignedIn::Entry(entry) => signed_in(shared, &entry),
    }
}

/// Signed in: added to the sources and shown, from a fresh source so its
/// new session is used even when it was shown already.
fn signed_in(shared: &Arc<Shared>, entry: &Value) -> std::result::Result<Value, Refused> {
    let kinds = &shared.config.kinds;
    let fail = |e: anyhow::Error| ("unavailable".to_owned(), format!("{e:#}"));
    let _changing = shared.changing.lock().unwrap();
    crate::storage::show_source(&shared.config.settings, kinds, Some(entry)).map_err(fail)?;
    let source = kinds.source_for(Some(entry)).map_err(fail)?;
    switch(shared, source.clone()).map_err(fail)?;
    eprintln!("pinholed: signed in to {}", source.id());
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
        // The source and its library change together for whoever reads
        // both (a fetch taking them for its job).
        let _context = shared.storage.library_moving.write().unwrap();
        *shared.source.write().unwrap() = source.clone();
        *shared.watch.lock().unwrap() = None;
        shared.rounds.lock().unwrap().push = false;
        *shared.health.lock().unwrap() = Health::Fine;
        *shared.progress.lock().unwrap() = None;
        let shown = if source.kind() == "none" { "nowhere".to_owned() } else { source.id() };
        eprintln!("pinholed: photos from {shown} now");
        shared.broadcast(source_event(&shared.config.kinds, &*source));
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
    watch(shared, &*source);
    Ok(())
}

/// Be told of the source's changes as they happen, if it can tell: a
/// round as soon as one comes (those within a moment of each other make
/// one, as a change comes as a few pushes), and windows hear whether it
/// is listening. Rounds every minute go on regardless.
fn watch(shared: &Arc<Shared>, source: &dyn Source) {
    let (weak, shown) = (Arc::downgrade(shared), shared.shown.load(Ordering::SeqCst));
    let pending = Arc::new(AtomicBool::new(false));
    let watching = source.watch(Box::new(move |heard| {
        let Some(shared) = weak.upgrade() else { return };
        // From a source shown before: its watch is stopping.
        if shared.shown.load(Ordering::SeqCst) != shown {
            return;
        }
        match heard {
            Heard::Changed => {
                if pending.swap(true, Ordering::SeqCst) {
                    return;
                }
                let pending = pending.clone();
                std::thread::spawn(move || {
                    std::thread::sleep(Duration::from_millis(1500));
                    pending.store(false, Ordering::SeqCst);
                    let _ = shared.sync_now.lock().unwrap().send(());
                });
            }
            Heard::Listening(listening) => {
                shared.rounds.lock().unwrap().push = listening;
                shared.broadcast(shared.rounds_event());
            }
        }
    }));
    if shared.shown.load(Ordering::SeqCst) == shown {
        *shared.watch.lock().unwrap() = watching;
    }
}

/// The settings or the plugins' switches changed under it (edited by
/// hand, a plugin turned on or off): the source they say is shown now, if
/// another. A window changing the source now is left to it.
fn follow_settings(shared: &Arc<Shared>) {
    let Ok(_changing) = shared.changing.try_lock() else { return };
    follow(shared, false);
}

/// The source the settings and the switches say, shown if it is not
/// (`Kinds::shown`), the caller holding `changing`. `switches`: the
/// plugins' switches read now, else only if plugins.json changed since.
/// Windows hear `kinds` if those changed, then the switch, then `sources`.
fn follow(shared: &Arc<Shared>, switches: bool) {
    let kinds = &shared.config.kinds;
    let turned = shared.refresh_kinds(switches);
    if turned {
        let all = kinds.all();
        let on: Vec<&str> = all.iter().map(|k| k.kind()).filter(|k| kinds.on(k)).collect();
        eprintln!("pinholed: source kinds on: {}", if on.is_empty() { "none".to_owned() } else { on.join(", ") });
        shared.broadcast(kinds_event(kinds));
    }
    let switched = match kinds.source_for(kinds.shown(&shared.settings()).as_ref()) {
        Ok(source) if source.id() != shared.source().id() => {
            if let Err(e) = switch(shared, source) {
                eprintln!("pinholed: switching source: {e:#}");
            }
            true
        }
        // The same source, of a provider whose plugin changed (its
        // manifest, say): its old kind stopped its process, so it is made
        // afresh of the new one, quietly, as after a panic.
        Ok(source) if turned && kinds.provided(source.kind()) => {
            {
                let _context = shared.storage.library_moving.write().unwrap();
                *shared.source.write().unwrap() = source.clone();
            }
            watch(shared, &*source);
            false
        }
        Ok(_) => false,
        Err(e) => {
            eprintln!("pinholed: the source the settings name: {e:#}");
            false
        }
    };
    if turned || switched {
        shared.broadcast(sources_event(shared));
    }
}

impl Shared {
    fn source(&self) -> Arc<dyn Source> {
        self.source.read().unwrap().clone()
    }

    /// The settings, as the kinds read them.
    fn settings(&self) -> crate::storage::Settings {
        crate::storage::read_settings(&self.config.settings, &self.config.kinds)
    }

    /// The kinds on as the plugins' switches say: read now (`now`), or
    /// only if plugins.json changed since they last were. Whether that
    /// turned any.
    fn refresh_kinds(&self, now: bool) -> bool {
        let stamp = crate::plugins::stamp(&self.config.plugins);
        let mut seen = self.plugins_seen.lock().unwrap();
        if !now && *seen == stamp {
            return false;
        }
        *seen = stamp;
        self.config.kinds.refresh(&crate::plugins::switches_in(&self.config.plugins))
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

    fn rounds_event(&self) -> Value {
        // A library given is never looked at for changes: no rounds.
        if self.config.given.is_some() {
            return rounds_event(&crate::source::NoSource, &Rounds::default());
        }
        rounds_event(&*self.source(), &self.rounds.lock().unwrap())
    }

    /// Send an event line to every window; ones that have gone are dropped.
    fn broadcast(&self, event: Value) {
        // Only queued: a window slow to read holds up no other.
        let line = event.to_string();
        self.listeners.lock().unwrap().retain(|out| out.send(&line));
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

    /// Ending with downloads still under way: their part files go, so
    /// none is left in the cache.
    fn abandon(&self) {
        for path in self.waiting.lock().unwrap().keys() {
            let _ = std::fs::remove_file(path.with_extension("part"));
            if let Some(extension) = path.extension().and_then(|e| e.to_str()) {
                let _ = std::fs::remove_file(path.with_extension(format!("part.{extension}")));
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
    if config.given.is_some() {
        eprintln!("pinholed: {} given: only read and fetched for, never synced", config.library.display());
    }

    let (lookup_tx, lookup_rx) = mpsc::channel::<Job>();
    let (sync_tx, sync_rx) = mpsc::channel::<()>();
    let (download_tx, download_rx) = mpsc::channel::<Job>();
    let (changes_tx, changes_rx) = mpsc::channel::<(String, Value, Reply)>();
    let (favorites_tx, favorites_rx) = mpsc::channel::<(Value, Reply)>();
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
        plugins_seen: Mutex::new(None),
        shown: std::sync::atomic::AtomicU64::new(0),
        rounds_shown: std::sync::atomic::AtomicU64::new(u64::MAX),
        rounds: Mutex::new(Rounds::default()),
        watch: Mutex::new(None),
        adding: Mutex::new(()),
        changing: Mutex::new(()),
        changes: Mutex::new(changes_tx),
        favorites: Mutex::new(favorites_tx),
        connections: Mutex::new((0, std::time::Instant::now())),
    });
    let given = shared.config.given;
    // Which kinds are on, before anything is listed or shown: the source
    // given was chosen by them already (bin/daemon.rs).
    shared.refresh_kinds(true);
    // A move of the folder cut short, put right before anything reads or
    // writes the library; one that cannot be, and the service does not
    // start, rather than work on a library not where the settings say.
    // The shared service's to do, not one of a library given.
    if given.is_none() {
        shared.storage.resume()?;
        crate::storage::forget_compatible();
    }
    // What services that ended were making from when they did: any
    // service's to do, as they share the cache.
    crate::storage::forget_making();

    {
        let shared = shared.clone();
        std::thread::spawn(move || lookup_loop(shared, lookup_rx));
    }
    let download_rx = Arc::new(Mutex::new(download_rx));
    for _ in 0..shared.config.downloads.max(1) {
        let (shared, rx) = (shared.clone(), download_rx.clone());
        std::thread::spawn(move || download_loop(shared, rx));
    }
    if given.is_none() {
        let storage = shared.storage.clone();
        std::thread::spawn(move || storage.evict_loop());
    }
    {
        if let Some(old) = shared.config.adopt.clone().filter(|_| given.is_none()) {
            shared.storage.adopt(old);
        }
        // Every window reads the library from its new place.
        let weak = Arc::downgrade(&shared);
        // With the folder of them all, when it moved: a window keeping to
        // another source's library (given --library) finds it there too.
        shared.storage.on_library_moved(move |library, libraries| {
            if let Some(shared) = weak.upgrade() {
                let mut event = json!({"event": "library_moved", "library": library});
                if let Some((from, to)) = libraries {
                    event["libraries"] = json!({"from": from, "to": to});
                }
                shared.broadcast(event);
            }
        });
    }
    if given.is_none() {
        {
            let shared = shared.clone();
            std::thread::spawn(move || sync_loop(shared, sync_rx));
        }
        watch(&shared, &*shared.source());
    }
    {
        let shared = shared.clone();
        std::thread::spawn(move || {
            for (method, params, reply) in changes_rx {
                let result = match method.as_str() {
                    "set_source" => set_source(&shared, &params),
                    "sign_out" => sign_out(&shared, &params),
                    // A plugin turned on or off in a window: what is
                    // listed and shown follows at once.
                    "plugins_changed" => {
                        let _changing = shared.changing.lock().unwrap();
                        follow(&shared, true);
                        Ok(json!({}))
                    }
                    _ => remove_source(&shared, &params),
                };
                match result {
                    Ok(value) => reply.send(Ok(value)),
                    Err((code, message)) => reply.send(Err((&code, message))),
                }
            }
        });
    }

    {
        let shared = shared.clone();
        std::thread::spawn(move || {
            for (params, reply) in favorites_rx {
                reply.send(set_favorite(&shared, &params));
            }
        });
    }

    let accepted = |stream: UnixStream| {
        shared.connections.lock().unwrap().0 += 1;
        let shared = shared.clone();
        std::thread::spawn(move || {
            if let Err(e) = serve(shared.clone(), stream) {
                eprintln!("pinholed: connection ended: {e:#}");
            }
            let mut connections = shared.connections.lock().unwrap();
            connections.0 -= 1;
            if connections.0 == 0 {
                connections.1 = std::time::Instant::now();
            }
        });
    };
    let Some(linger) = given else {
        for stream in listener.incoming() {
            let Ok(stream) = stream else { continue };
            accepted(stream);
        }
        return Ok(());
    };
    // A library given: until no window has been connected for a while.
    listener.set_nonblocking(true)?;
    loop {
        match listener.accept() {
            Ok((stream, _)) => {
                if stream.set_nonblocking(false).is_ok() {
                    accepted(stream);
                }
            }
            Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => {
                let (count, since) = *shared.connections.lock().unwrap();
                // Downloads under way end first, half a minute at most: what
                // they bring is in the shared cache, not left half there.
                let downloading = shared.in_flight.load(Ordering::Relaxed) > 0;
                if count == 0 && since.elapsed() >= linger && (!downloading || since.elapsed() >= linger + FINISH_WITHIN) {
                    eprintln!("pinholed: no window for {} s: done", since.elapsed().as_secs());
                    // A window coming meanwhile finds none, and starts one.
                    let _ = std::fs::remove_file(&shared.config.socket);
                    shared.abandon();
                    return Ok(());
                }
                std::thread::sleep(Duration::from_millis(50));
            }
            Err(_) => std::thread::sleep(Duration::from_millis(50)),
        }
    }
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
    let out = Window::open(&stream)?;
    let served = talk(shared.clone(), stream, out.clone());
    // Gone, however it went: no more news for it.
    shared.listeners.lock().unwrap().retain(|other| !Arc::ptr_eq(other, &out));
    served
}

/// A window's requests, answered until it hangs up (or is hung up on).
fn talk(shared: Arc<Shared>, stream: UnixStream, out: Arc<Window>) -> Result<()> {
    // First which protocol this is, for a window of another version to
    // tell (docs/service.md, Transport): before it hears any news.
    out.send(json!({"event": "hello", "protocol": PROTOCOL, "version": env!("CARGO_PKG_VERSION")}));
    {
        // A window that connects hears where the photos come from, and which
        // library to read (its own guess may be another): told before any
        // switch or move after, which is told it in turn, never after one
        // it would undo.
        let _context = shared.storage.library_moving.read().unwrap();
        shared.listeners.lock().unwrap().push(out.clone());
        out.send(source_event(&shared.config.kinds, &*shared.source()));
        out.send(json!({"event": "library_moved", "library": shared.storage.library()}));
    }
    // And if something is wrong, that too.
    out.send(sources_event(&shared));
    out.send(kinds_event(&shared.config.kinds));
    let health = *shared.health.lock().unwrap();
    if health != Health::Fine {
        out.send(health_event(health));
    }
    if let Some(progress) = shared.progress.lock().unwrap().clone() {
        out.send(progress);
    }
    out.send(shared.rounds_event());
    // Opened when first needed: before the first round has built the
    // library there is none, and the connection must stay up regardless.
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
                Reply { id: Value::Null, out: Out::Window(out.clone()), stream: false }.send(Err(("bad_request", "not JSON".into())));
                continue;
            }
        };
        let stream = request["params"]["stream"].as_bool() == Some(true);
        let reply = Reply { id: request["id"].clone(), out: Out::Window(out.clone()), stream };
        // A library given is only read: nothing changed from here.
        if let Some(method) = request["method"].as_str().filter(|m| shared.config.given.is_some() && CHANGES.contains(m)) {
            let library = shared.storage.library();
            reply.send(Err(("read_only", format!("{method}: {} is a library given to a window, only read", library.display()))));
            continue;
        }
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
                "windows": shared.listeners.lock().unwrap().len(),
                "health": shared.health.lock().unwrap().name(),
                "source": source_info(&shared.config.kinds, &*shared.source()),
                "progress": shared.progress.lock().unwrap().clone().map(|mut p| {
                    p.as_object_mut().map(|o| o.remove("event"));
                    p
                }),
                "rounds": Some(shared.rounds_event()).map(|mut r| {
                    r.as_object_mut().map(|o| o.remove("event"));
                    r
                }),
            }))),
            Some("sources") => reply.send(Ok(sources_report(&shared))),
            Some("kinds") => reply.send(Ok(shared.config.kinds.report())),
            // A switch may wait a moment for a library being built to
            // give up: done in turn, off this connection's thread.
            Some(method @ ("set_source" | "remove_source" | "sign_out" | "plugins_changed")) => {
                let _ = shared.changes.lock().unwrap().send((method.to_owned(), request["params"].clone(), reply));
            }
            // These take a while (the sign-in provider starts, a server
            // answers, a disk is looked through): on a thread of their
            // own, so this window's photos keep coming meanwhile.
            // Minutes for a long video to iCloud: on a thread of its own.
            Some("add") => {
                let (shared, params) = (shared.clone(), request["params"].clone());
                std::thread::spawn(move || match add(&shared, &params, &reply) {
                    Ok(value) => reply.send(Ok(value)),
                    Err((code, message)) => reply.send(Err((code, message))),
                });
            }
            // A second or so for iCloud: in turn, off this connection's
            // thread.
            Some("set_favorite") => {
                let _ = shared.favorites.lock().unwrap().send((request["params"].clone(), reply));
            }
            Some(method @ ("sign_in" | "submit_code" | "discover")) => {
                let (shared, params, method) = (shared.clone(), request["params"].clone(), method.to_owned());
                std::thread::spawn(move || {
                    let result = match method.as_str() {
                        "discover" => Ok(discover(&shared)),
                        _ => sign_in(&shared, &method, &params),
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
                // The index read, the source and the library taken for the
                // job all of one moment: no switch or move between them.
                let _context = shared.storage.library_moving.read().unwrap();
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

/// Add the files a window dropped (`files`, absolute paths; folders of
/// them too) to the source shown, one asset at a time, a Live Photo's two
/// files as one (`add::gather`). The window hears `adding` as it goes, and
/// how each asset went in the answer. Each asset added starts a round, so
/// it shows as soon as the source has it.
fn add(shared: &Shared, params: &Value, reply: &Reply) -> std::result::Result<Value, (&'static str, String)> {
    let files: Vec<PathBuf> = params["files"]
        .as_array()
        .map(|a| a.iter().filter_map(Value::as_str).map(PathBuf::from).collect())
        .unwrap_or_default();
    if files.is_empty() || files.iter().any(|f| f.is_relative()) {
        return Err(("bad_request", "add needs files, as absolute paths".into()));
    }
    let source = shared.source();
    let accepts = source.accepts();
    if accepts.is_empty() {
        return Err(("unsupported", format!("{} cannot be added to", if source.kind() == "none" { "Nothing".into() } else { source.name() })));
    }
    let (additions, other) = crate::add::gather(&files);
    let mut results: Vec<Value> = other
        .iter()
        .map(|f| json!({"file": f, "files": [f], "state": "failed", "error": {"code": "unsupported", "message": "not a photo or a video"}}))
        .collect();
    let bytes: u64 = additions.iter().map(|a| a.size()).sum();
    let (total, id) = (additions.len(), source.id());
    let progress = |done: usize, file: Option<&Path>, sent: u64| {
        json!({"event": "adding", "request": reply.id, "source": id, "done": done, "total": total,
               "file": file, "sent": sent, "bytes": bytes})
    };
    // Another window's drop first, if one is being added.
    let _one_at_a_time = shared.adding.lock().unwrap();
    let mut sent_before = 0u64;
    let said = Mutex::new(std::time::Instant::now() - Duration::from_secs(1));
    for (i, addition) in additions.iter().enumerate() {
        reply.event(progress(i, Some(&addition.file), sent_before));
        let mut result = json!({"file": addition.file, "files": addition.files().collect::<Vec<_>>()});
        let outcome = if accepts.contains(&crate::add::extension(&addition.file).as_str()) {
            source.add(addition, &|sent, _| {
                // At most four times a second.
                let mut said = said.lock().unwrap();
                if said.elapsed() >= Duration::from_millis(250) {
                    *said = std::time::Instant::now();
                    reply.event(progress(i, Some(&addition.file), sent_before + sent));
                }
            })
        } else {
            let name = addition.file.file_name().unwrap_or_default().to_string_lossy().into_owned();
            Err(crate::add::AddError::new("unsupported", format!("{} does not take {name}", source.name())).into())
        };
        sent_before += addition.size();
        match outcome {
            Ok(added) => {
                result["state"] = json!(if added.duplicate { "duplicate" } else { "added" });
                result["asset"] = json!(added.id);
                if let Some(note) = added.note {
                    result["note"] = json!(note);
                }
                shared.reached();
                let _ = shared.sync_now.lock().unwrap().send(());
            }
            Err(e) => {
                eprintln!("pinholed: add {}: {e:#}", addition.file.display());
                let code = match e.downcast_ref::<crate::add::AddError>() {
                    Some(a) => a.code,
                    None => shared.failed(&e),
                };
                result["state"] = json!("failed");
                result["error"] = json!({"code": code, "message": format!("{e:#}")});
            }
        }
        results.push(result);
    }
    reply.event(progress(total, None, bytes));
    let count = |state: &str| results.iter().filter(|r| r["state"] == state).count();
    Ok(json!({"source": id, "added": count("added"), "duplicates": count("duplicate"), "failed": count("failed"), "files": results}))
}

/// Make assets favourites, or not (`assets`, `favorite`): in the source
/// first (iCloud's records, a folder's library), then in the index, so
/// every window shows it at once and the next round agrees. Those already
/// so are left alone. pinhole-fake's library, which no source fills, keeps
/// them itself.
fn set_favorite(shared: &Shared, params: &Value) -> std::result::Result<Value, (&'static str, String)> {
    let ids: Vec<String> = params["assets"]
        .as_array()
        .map(|a| a.iter().filter_map(Value::as_str).map(str::to_owned).collect())
        .unwrap_or_default();
    let (false, Some(favorite)) = (ids.is_empty(), params["favorite"].as_bool()) else {
        return Err(("bad_request", "set_favorite needs assets and favorite".into()));
    };
    // Not under the library's lock, which a sync round holds throughout:
    // SQLite has the index's own writes wait for each other. Not while the
    // library is copied elsewhere, though, where it would be lost, nor
    // while another source is switched to: the source and its library are
    // the same one's.
    let _moving = shared.storage.library_moving.read().unwrap();
    let source = shared.source();
    let path = shared.storage.library().join("library.sqlite");
    if !path.exists() {
        return Err(("not_found", "there is no library yet".into()));
    }
    let db = crate::db::open(&path).map_err(|e| ("unavailable", format!("{e:#}")))?;
    // Asked of one source's photos (a window's undo, say): never done to
    // another's of the same ids, shown since.
    if let Some(asked) = params["source"].as_str() {
        if asked.is_empty() || crate::db::meta(&db, "source").as_deref() != Some(asked) {
            return Err(("other_source", format!("{asked} is not the source shown")));
        }
    }
    let mut wanted = Vec::with_capacity(ids.len());
    for id in ids {
        let now: Option<bool> = db.query_row("SELECT favorite FROM assets WHERE id = ?1", [&id], |r| r.get(0)).ok();
        match now {
            None => return Err(("not_found", format!("the library has no {id}"))),
            Some(now) if now != favorite => wanted.push(id),
            Some(_) => {}
        }
    }
    if wanted.is_empty() {
        return Ok(json!({"changed": 0}));
    }
    match crate::db::meta(&db, "source") {
        Some(owner) if owner == "fake" => {}
        Some(owner) if owner == source.id() => {
            if let Err(e) = source.set_favorite(&db, &wanted, favorite) {
                eprintln!("pinholed: set_favorite: {e:#}");
                return Err(match e.downcast_ref::<Unsupported>() {
                    Some(u) => ("unsupported", u.0.clone()),
                    None => (shared.failed(&e), format!("{e:#}")),
                });
            }
            shared.reached();
        }
        _ => return Err(("unsupported", format!("{} is not the library shown", path.display()))),
    }
    let version = (|| -> Result<i64> {
        let tx = db.unchecked_transaction()?;
        for id in &wanted {
            tx.execute("UPDATE assets SET favorite = ?2 WHERE id = ?1", params![id, favorite])?;
        }
        let version = crate::db::meta(&tx, "library_version").and_then(|v| v.parse().ok()).unwrap_or(0) + 1;
        crate::db::set_meta(&tx, "library_version", &version.to_string())?;
        tx.commit()?;
        Ok(version)
    })()
    .map_err(|e| ("unavailable", format!("{e:#}")))?;
    eprintln!("pinholed: favourite {} {} assets", if favorite { "set on" } else { "taken off" }, wanted.len());
    shared.broadcast(json!({"event": "library_changed", "version": version, "added": 0, "removed": 0}));
    Ok(json!({"changed": wanted.len()}))
}

fn fetch(shared: &Arc<Shared>, db: &Connection, params: &Value, reply: Reply) {
    let (Some(asset), Some(kind)) = (params["asset"].as_str(), params["kind"].as_str()) else {
        return reply.send(Err(("bad_request", "fetch needs asset and kind".into())));
    };
    let Some(want) = Want::parse(kind).filter(|w| *w != Want::Thumb) else {
        return reply.send(Err(("bad_request", format!("unknown kind {kind}"))));
    };
    // Asked of one source's photo (the window's library, as it says): never
    // another's of the same id, shown since.
    // One that says whose it is not, "", is taken for no one's. And the
    // library read must be the source's it is fetched from: a library given
    // to this service (--library) may be another's, its names those of
    // another's files.
    let owner = crate::db::meta(db, "source");
    if let Some(asked) = params["source"].as_str() {
        if asked.is_empty() || owner.as_deref() != Some(asked) {
            return reply.send(Err(("other_source", format!("{asked:?} is not the source shown"))));
        }
    }
    if owner != Some(shared.source().id()) {
        return reply.send(Err(("other_source", format!("the library read holds the photos of {}", owner.as_deref().unwrap_or("no one")))));
    }
    if want == Want::Jpeg {
        return jpeg(shared, db, asset, Reply { stream: false, ..reply });
    }

    // The rendition that serves this kind, as the catalog chose it.
    let row = db.query_row(
        "SELECT name, key, coalesce(file_type, '') FROM renditions WHERE asset_id = ?1 AND kind = ?2",
        params![asset, kind],
        |r| Ok(Stored { asset: asset.to_owned(), want, name: r.get(0)?, key: r.get(1)?, file_type: r.get(2)? }),
    );
    let stored = match row {
        Ok(stored) => stored,
        // An effect the source has no render of: a loop is the motion
        // itself, the others are made from it.
        Err(_) if want == Want::Loop => return fetch(shared, db, &json!({"asset": asset, "kind": "live"}), reply),
        Err(_) if want.effect().is_some() => return make(shared, db, asset, want, reply),
        Err(_) => return reply.send(Err(("not_found", format!("{asset} has no {kind} rendition")))),
    };
    // A video given a cover of its own (cover.rs) has the same frame in
    // place of the source's medium, which shows its first.
    if want == Want::Medium {
        let thumb: Option<String> =
            db.query_row("SELECT thumb_path FROM assets WHERE id = ?1", [asset], |r| r.get(0)).ok().flatten();
        if let Some(cover) = thumb.filter(|t| t.ends_with("-cover.jpg")) {
            let path = shared.storage.library().join(crate::sync::medium_cover_path(&cover));
            if path.exists() {
                return reply.send(Ok(json!({"path": path, "type": "public.jpeg", "rendition": "cover", "cached": true})));
            }
        }
    }
    let (rendition, file_type) = (stored.name.clone(), stored.file_type.clone());
    // From here on the source and library it was asked of, whatever is
    // shown by the time it comes.
    let (source, library) = (shared.source(), shared.storage.library());
    let library = library.strip_prefix(shared.storage.location()).map_or(library.clone(), Path::to_path_buf);
    // On this computer already: served as it is.
    if let Some(path) = source.local(&stored) {
        return reply.send(if path.exists() {
            Ok(json!({"path": path, "type": file_type, "rendition": rendition, "cached": true}))
        } else {
            Err(("not_found", format!("{} is not there", path.display())))
        });
    }
    let relative = cached_path(want, &stored.key, extension(&file_type), false);
    if let Some(path) = shared.storage.find(&relative) {
        touch(&path);
        return reply.send(Ok(json!({"path": path, "type": file_type, "rendition": rendition, "cached": true})));
    }
    // Turned down lately, and not changed since: said again at once.
    if let Some(why) = crate::source::unreadable(&source.id(), &stored.key) {
        return reply.send(Err(("unreadable", why)));
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

    let _ = shared.lookups.lock().unwrap().send(Job { source, library, stored, path, ticket: None, retried: false });
}

/// Where in the cache a file of this kind and key goes. The effects go
/// with the motion (storage.rs counts them as `live`), except a long
/// exposure the source has, which is the photo itself (iCloud's JPEGFull):
/// the same file as `full`. Made ones are named after the key of what they
/// are made from: the motion's, or for a `jpeg` the `full` it goes beside
/// (and is counted with).
fn cached_path(want: Want, key: &str, extension: &str, made: bool) -> PathBuf {
    let folder = match want {
        Want::LongExposure if !made => Want::Full,
        Want::Loop | Want::Bounce | Want::LongExposure => Want::Live,
        Want::Jpeg => Want::Full,
        other => other,
    };
    let suffix = if made { format!("-{}", want.name()) } else { String::new() };
    Path::new(folder.name()).join(thumb_path(key).replacen("thumbs/", "", 1).replace(".jpg", &format!("{suffix}.{extension}")))
}

/// `full` as a JPEG: `full` itself when it is one, else one made from it.
/// A video has none.
fn jpeg(shared: &Arc<Shared>, db: &Connection, asset: &str, reply: Reply) {
    let full = db.query_row(
        "SELECT coalesce(r.file_type, ''), a.is_video FROM renditions r JOIN assets a ON a.id = r.asset_id
         WHERE r.asset_id = ?1 AND r.kind = 'full'",
        [asset],
        |r| Ok((r.get::<_, String>(0)?, r.get::<_, bool>(1)?)),
    );
    match full {
        Err(_) => reply.send(Err(("not_found", format!("{asset} has no full rendition to make a JPEG of")))),
        Ok((_, true)) => reply.send(Err(("not_found", format!("{asset} is a video: it has no JPEG")))),
        Ok((file_type, _)) if file_type == "public.jpeg" => fetch(shared, db, &json!({"asset": asset, "kind": "full"}), reply),
        Ok(_) => make(shared, db, asset, Want::Jpeg, reply),
    }
}

/// A file made from another into the cache beside it: an effect from the
/// motion (effects.rs, ffmpeg), a `jpeg` from `full` (jpeg.rs, ImageMagick).
/// What it is made from is fetched as a window would, then it is made in a
/// thread of its own. Requests for one being made wait for it.
fn make(shared: &Arc<Shared>, db: &Connection, asset: &str, want: Want, reply: Reply) {
    let from = if want == Want::Jpeg { Want::Full } else { Want::Live };
    let made_from = db.query_row(
        "SELECT key FROM renditions WHERE asset_id = ?1 AND kind = ?2",
        params![asset, from.name()],
        |r| r.get::<_, String>(0),
    );
    let Ok(key) = made_from else {
        return reply.send(Err(("not_found", format!("{asset} has no {} to make a {} of", from.name(), want.name()))));
    };
    let (file_type, extension) = match want {
        Want::Bounce => ("public.mpeg-4", "mp4"),
        _ => ("public.jpeg", "jpg"),
    };
    let relative = cached_path(want, &key, extension, true);
    if let Some(path) = shared.storage.find(&relative) {
        touch(&path);
        return reply.send(Ok(json!({"path": path, "type": file_type, "rendition": "made", "cached": true})));
    }
    let path = shared.storage.cache().join(&relative);
    {
        let mut waiting = shared.waiting.lock().unwrap();
        if let Some((_, replies)) = waiting.get_mut(&path) {
            replies.push(reply);
            return;
        }
        let info = json!({"path": path, "type": file_type, "rendition": "made", "cached": false});
        waiting.insert(path.clone(), (info, vec![reply]));
        shared.in_flight.fetch_add(1, Ordering::Relaxed);
    }
    let (tx, rx) = mpsc::channel();
    fetch(shared, db, &json!({"asset": asset, "kind": from.name()}), Reply { id: Value::Null, out: Out::Service(tx), stream: false });
    let shared = shared.clone();
    std::thread::spawn(move || {
        let made = (|| -> std::result::Result<(), (String, String)> {
            let fetched = rx.recv().map_err(|_| ("unavailable".to_owned(), format!("the {} never came", from.name())))??;
            let source = PathBuf::from(fetched["path"].as_str().unwrap_or_default());
            if let Some(dir) = path.parent() {
                std::fs::create_dir_all(dir).map_err(|e| ("unavailable".to_owned(), e.to_string()))?;
            }
            // Made beside the target and renamed: a reader never sees half a file.
            let part = path.with_extension(format!("part.{extension}"));
            let result = match want {
                Want::Bounce => crate::effects::bounce(&source, &part),
                Want::Jpeg => crate::jpeg::make(&source, &part),
                _ => crate::effects::long_exposure(&source, &part),
            }
            .and_then(|()| Ok(std::fs::rename(&part, &path)?));
            if let Err(e) = result {
                let _ = std::fs::remove_file(&part);
                eprintln!("pinholed: making the {} of {}: {e:#}", want.name(), source.display());
                return Err(("unavailable".to_owned(), format!("{e:#}")));
            }
            Ok(())
        })();
        shared.finish(&path, made.as_ref().map(|_| ()).map_err(|(code, message)| (code.as_str(), message.clone())));
    });
}

/// Gathers requests for a moment and has the source get them all ready in
/// one go (iCloud looks their links up together), then passes them to the
/// downloads.
fn lookup_loop(shared: Arc<Shared>, rx: mpsc::Receiver<Job>) {
    // Ready at start: iCloud's provider takes a few seconds to check the
    // session, which the first request should not have to wait for. Not
    // for a library given, fetched for seldom if at all (pinhole-fake's
    // never is): its source starts as its first photo is asked for.
    if shared.config.given.is_none() {
        if let Err(e) = shared.source().start() {
            eprintln!("pinholed: {} is not ready yet: {e:#}", shared.source().name());
            shared.failed(&e);
        }
    }
    let mut dbs: HashMap<PathBuf, (u64, Connection)> = HashMap::new();
    while let Ok(first) = rx.recv() {
        std::thread::sleep(BATCH_WINDOW);
        let mut batch = vec![first];
        while batch.len() < BATCH_MAX {
            match rx.try_recv() {
                Ok(job) => batch.push(job),
                Err(_) => break,
            }
        }
        // Each source by the library it was asked of: almost always the one
        // shown, unless another was shown as these were asked.
        let mut groups: Vec<Vec<Job>> = Vec::new();
        for job in batch {
            match groups.iter_mut().find(|g| g[0].source.id() == job.source.id() && g[0].library == job.library) {
                Some(group) => group.push(job),
                None => groups.push(vec![job]),
            }
        }
        for group in groups {
            lookup(&shared, &mut dbs, group);
        }
    }
}

/// Get a batch of one source's jobs ready (`prepare`), and hand them to
/// the downloads.
fn lookup(shared: &Shared, dbs: &mut HashMap<PathBuf, (u64, Connection)>, batch: Vec<Job>) {
    // Nothing moves the library while it is opened and written (fresh
    // links); where it is now, as it may have moved since it was asked.
    let _moving = shared.storage.library_moving.read().unwrap();
    let source = batch[0].source.clone();
    let library = shared.storage.location().join(&batch[0].library);
    // Only the source shown says how the source is doing.
    let shown = source.id() == shared.source().id();
    let failed = |e: &anyhow::Error| if shown { shared.failed(e) } else { "unavailable" };
    // The source may keep what it learns in the index (iCloud its fresh
    // links), so the next request needs no lookup. Opened again once the
    // library has moved. A library given is only read: opened so, no
    // journal mode set nor table made in it, and the source keeps what it
    // learns in memory.
    let generation = shared.storage.library_generation.load(Ordering::Relaxed);
    if dbs.get(&library).is_none_or(|(g, _)| *g != generation) {
        dbs.remove(&library);
        let path = library.join("library.sqlite");
        let db = if shared.config.given.is_some() {
            Connection::open_with_flags(&path, OpenFlags::SQLITE_OPEN_READ_ONLY).map_err(anyhow::Error::from)
        } else {
            crate::db::open(&path)
        };
        if let Ok(db) = db {
            dbs.insert(library.clone(), (generation, db));
        }
    }
    let Some((_, db)) = dbs.get(&library) else {
        for job in batch {
            shared.finish(&job.path, Err(("not_found", "there is no library yet".into())));
        }
        return;
    };
    let stored: Vec<Stored> = batch.iter().map(|j| j.stored.clone()).collect();
    let tickets = match source.prepare(db, &stored) {
        Ok(t) => t,
        Err(e) => {
            let code = failed(&e);
            for job in batch {
                shared.finish(&job.path, Err((code, format!("{e:#}"))));
            }
            return;
        }
    };
    if shown {
        shared.set_health(Health::Fine);
    }
    for (mut job, ticket) in batch.into_iter().zip(tickets) {
        match ticket {
            Ok(ticket) => {
                job.ticket = Some(ticket);
                let _ = shared.downloads.lock().unwrap().send(job);
            }
            Err(FetchError::Failed(e)) => {
                let code = failed(&e);
                shared.finish(&job.path, Err((code, format!("{e:#}"))))
            }
            Err(FetchError::Unreadable(why)) => unreadable(shared, &job, why),
            Err(e) => shared.finish(&job.path, Err(("not_found", e.to_string()))),
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
            // The file's alone: the source's health stays as it is.
            Err(FetchError::Unreadable(why)) => unreadable(&shared, &job, why),
            Err(FetchError::Failed(e)) => {
                let code = shared.failed(&e);
                shared.finish(&job.path, Err((code, format!("{e:#}"))))
            }
        }
    }
}

/// A file turned down by what makes an image of it: kept in mind, so that
/// it is not made again until it changes (source::unreadable), and said.
fn unreadable(shared: &Shared, job: &Job, why: String) {
    eprintln!("pinholed: {} of {}: {why}", job.stored.want.name(), job.stored.asset);
    crate::source::note_unreadable(&job.source.id(), &job.stored.key, &why);
    shared.finish(&job.path, Err(("unreadable", why)));
}

fn download(shared: &Shared, job: &Job) -> std::result::Result<(), FetchError> {
    let source = job.source.clone();
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
        // Windows say it is being looked at now.
        shared.rounds.lock().unwrap().next = None;
        shared.broadcast(shared.rounds_event());
        let (source, shown) = (shared.source(), shared.shown.load(Ordering::SeqCst));
        // A round that panics (a bug in a source, bad data it trips on) is
        // a round that failed, not the end of every round after it.
        match std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| sync_round(&shared))) {
            Ok(Ok(())) => {
                shared.set_health(Health::Fine);
                // Not a round given up as another source was shown.
                if shared.shown.load(Ordering::SeqCst) == shown {
                    shared.rounds.lock().unwrap().last.insert(source.id(), SystemTime::now());
                }
            }
            Ok(Err(e)) => {
                eprintln!("pinholed: sync: {e:#}");
                shared.failed(&e);
            }
            Err(panic) => panicked(&shared, &*source, shown, &*panic),
        }
        // Until the next round, or a window asking for one now.
        let every = shared.config.sync_every.map_or_else(|| shared.source().rounds_every(), Duration::from_secs);
        let every = every.max(Duration::from_secs(5));
        shared.rounds.lock().unwrap().next = Some(SystemTime::now() + every);
        shared.broadcast(shared.rounds_event());
        match wake.recv_timeout(every) {
            Ok(()) | Err(mpsc::RecvTimeoutError::Timeout) => {}
            Err(mpsc::RecvTimeoutError::Disconnected) => return,
        }
        // Several asks at once make one round.
        while wake.try_recv().is_ok() {}
    }
}

/// A sync round panicked: logged, the health `failing`, and what the panic
/// left behind put right for the next round. The library's lock it held
/// is taken as free (the index's own transactions roll back what was half
/// written); a library being built says it is done for now; and the
/// source, whose own state it may have left half changed (a lock held
/// mid-listing), is made afresh from its entry, quietly: it is the same
/// source, so windows have nothing to read again.
fn panicked(shared: &Arc<Shared>, source: &dyn Source, shown: u64, panic: &(dyn std::any::Any + Send)) {
    let what = panic
        .downcast_ref::<String>()
        .map(String::as_str)
        .or_else(|| panic.downcast_ref::<&str>().copied())
        .unwrap_or("no message");
    eprintln!("pinholed: sync: the round for {} panicked ({what}); the next one goes on", source.id());
    shared.storage.library_lock.clear_poison();
    if shared.progress.lock().unwrap_or_else(|e| e.into_inner()).take().is_some() {
        shared.broadcast(json!({"event": "progress", "source": source.id(), "phase": "done"}));
    }
    shared.set_health(Health::Failing);
    let kinds = &shared.config.kinds;
    let Some(entry) = kinds.shown(&shared.settings()).filter(|e| kinds.id_of(e) == source.id()) else { return };
    let Ok(fresh) = kinds.source_for(Some(&entry)) else { return };
    let _context = shared.storage.library_moving.write().unwrap_or_else(|e| e.into_inner());
    if shared.shown.load(Ordering::SeqCst) == shown {
        *shared.source.write().unwrap_or_else(|e| e.into_inner()) = fresh;
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
    // Nothing is made for a source not ready, such as pinhole.icloud's
    // with no account signed in to yet.
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
        // Thumbnails for what is new or edited (its old one forgotten: a
        // folder's file replaced), and on the first round since the
        // source was shown, for whatever an earlier round left without
        // one (it gave up when another source was shown). Not every round,
        // and never again for a file turned down until it changes
        // (source::unreadable).
        let fresh = added > 0 || report.changes.reasons.contains_key("edited") || first;
        let missing: i64 = if fresh {
            db.query_row(
                "SELECT count(*) FROM assets a JOIN renditions r ON r.asset_id = a.id AND r.kind = 'thumb'
                 WHERE a.thumb_path IS NULL",
                [],
                |r| r.get(0),
            )?
        } else {
            0
        };
        let version: i64 = crate::db::meta(&db, "library_version").and_then(|v| v.parse().ok()).unwrap_or(0);
        let announce = |added: usize, removed: usize| {
            shared.broadcast(json!({"event": "library_changed", "version": version, "added": added, "removed": removed}));
        };
        let mut made = 0;
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
            made = crate::sync::make_thumbs(source.as_ref(), &mut db, &library, 16, &|done, total| {
                if telling {
                    building.step("thumbs", done as u64, total as u64);
                }
            }, &stop)?
            .made;
        }
        // A video whose thumbnail shows next to nothing gets a cover of its
        // own (cover.rs), looked for as thumbnails are.
        let covers = if fresh { crate::sync::make_covers(source.as_ref(), &mut db, &library, &stop)? } else { 0 };
        if !report.changes.visible() && made == 0 && covers == 0 {
            return Ok(());
        }
        eprintln!(
            "pinholed: sync: library version {version}: {added} new, {removed} removed, {made} of {missing} thumbnails made, {covers} covers made"
        );
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

#[cfg(test)]
mod tests {
    use super::*;

    /// A folder as the rounds see it: its id, and nothing in it.
    struct Pictures;

    impl Source for Pictures {
        fn name(&self) -> String {
            "Pictures".into()
        }

        fn kind(&self) -> &'static str {
            "folder"
        }

        fn id(&self) -> String {
            "folder:/home/me/Pictures".into()
        }

        fn library_name(&self) -> String {
            "home-me-Pictures".into()
        }

        fn list(&self, _index: &mut crate::db::Index, _progress: &(dyn Fn(u64, u64) + Sync)) -> Result<crate::source::Listing> {
            Ok(Default::default())
        }

        fn changes(&self, _index: &mut crate::db::Index, _token: &str) -> Result<crate::source::Listing> {
            Ok(Default::default())
        }

        fn prepare(&self, _index: &Connection, wanted: &[Stored]) -> Result<Vec<Result<Ticket, FetchError>>> {
            Ok(wanted.iter().map(|_| Err(FetchError::NotFound("nothing here".into()))).collect())
        }

        fn open(&self, _ticket: &Ticket, _range: Option<(u64, u64)>) -> Result<crate::source::Body, FetchError> {
            Err(FetchError::NotFound("nothing here".into()))
        }
    }

    #[test]
    fn rounds_say_when_the_source_shown_was_looked_at_and_is_next() {
        let folder = Pictures;
        let checked = SystemTime::UNIX_EPOCH + Duration::from_secs(1_000);
        let mut rounds = Rounds::default();
        rounds.last.insert("folder:/home/elsewhere".into(), checked);
        // Never looked at, and being looked at now.
        let event = rounds_event(&folder, &rounds);
        assert_eq!(event["source"], "folder:/home/me/Pictures");
        assert!(event["last"].is_null() && event["next"].is_null());

        rounds.last.insert(folder.id(), checked);
        rounds.next = Some(checked + Duration::from_secs(10));
        let event = rounds_event(&folder, &rounds);
        assert_eq!(event["last"], 1_000_000);
        assert_eq!(event["next"], 1_010_000);
        assert_eq!(event["push"], false);
        rounds.push = true;
        assert_eq!(rounds_event(&folder, &rounds)["push"], true);

        let none = rounds_event(&crate::source::NoSource, &rounds);
        assert!(none["source"].is_null() && none["next"].is_null());
    }
}
