//! A service of a library given to a window ends a while after its last
//! window, whatever went on (docs/service.md): a bug answering a window
//! (a panic) ends that window's connection, not the count of windows; one
//! in getting a file answers it with an error rather than leave it under
//! way; another service started on its socket's path (its own removed)
//! keeps its socket; and a program still making something as it ends is
//! stopped, its part files gone. The shared service never ends on its own.
//!
//! One test, its parts one after another: the programs running and the
//! cache are the process's, as they are pinholed's.

use std::io::{BufRead, BufReader, Write};
use std::os::unix::net::UnixStream;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{mpsc, Arc};
use std::time::{Duration, Instant};

use anyhow::Result;
use pinhole_core::db::Index;
use pinhole_core::service::{self, Config};
use pinhole_core::source::{Body, FetchError, Listing, Source, Stored, Ticket};
use pinhole_folder::FolderSource;
use serde_json::{json, Value};

/// A folder whose `local` or `open` panics while `PANICS` says so, as a
/// bug in a source would.
struct Faulty {
    folder: FolderSource,
    panics_in: &'static str,
}

static PANICS: AtomicBool = AtomicBool::new(true);

impl Faulty {
    fn trip(&self, method: &str) {
        if self.panics_in == method && PANICS.load(Ordering::SeqCst) {
            panic!("a source tripped over its own feet in {method}");
        }
    }
}

impl Source for Faulty {
    fn name(&self) -> String {
        self.folder.name()
    }
    fn kind(&self) -> &'static str {
        self.folder.kind()
    }
    fn id(&self) -> String {
        self.folder.id()
    }
    fn library_name(&self) -> String {
        self.folder.library_name()
    }
    fn list(&self, index: &mut Index, progress: &(dyn Fn(u64, u64) + Sync)) -> Result<Listing> {
        self.folder.list(index, progress)
    }
    fn changes(&self, index: &mut Index, token: &str) -> Result<Listing> {
        self.folder.changes(index, token)
    }
    fn local(&self, rendition: &Stored) -> Option<PathBuf> {
        self.trip("local");
        self.folder.local(rendition)
    }
    fn prepare(&self, index: &rusqlite::Connection, wanted: &[Stored]) -> Result<Vec<Result<Ticket, FetchError>>> {
        self.folder.prepare(index, wanted)
    }
    fn open(&self, ticket: &Ticket, range: Option<(u64, u64)>) -> Result<Body, FetchError> {
        self.trip("open");
        self.folder.open(ticket, range)
    }
}

struct Window {
    out: UnixStream,
    lines: BufReader<UnixStream>,
    next: u64,
}

impl Window {
    fn connect(socket: &Path) -> Window {
        let deadline = Instant::now() + Duration::from_secs(10);
        let stream = loop {
            match UnixStream::connect(socket) {
                Ok(s) => break s,
                Err(_) if Instant::now() < deadline => std::thread::sleep(Duration::from_millis(50)),
                Err(e) => panic!("no service on {}: {e}", socket.display()),
            }
        };
        stream.set_read_timeout(Some(Duration::from_millis(200))).unwrap();
        Window { out: stream.try_clone().unwrap(), lines: BufReader::new(stream), next: 1 }
    }

    /// A line, none within the read timeout; Err once hung up on.
    fn line(&mut self) -> std::result::Result<Option<Value>, ()> {
        let mut line = String::new();
        match self.lines.read_line(&mut line) {
            Ok(0) => Err(()),
            Ok(_) => Ok(Some(serde_json::from_str(&line).unwrap())),
            Err(_) => Ok(None),
        }
    }

    fn send(&mut self, method: &str, params: Value) -> u64 {
        let id = self.next;
        self.next += 1;
        writeln!(self.out, "{}", json!({"id": id, "method": method, "params": params})).unwrap();
        id
    }

    /// Its answer within `secs`; None if hung up on first.
    fn answer(&mut self, id: u64, secs: u64) -> Option<Value> {
        let deadline = Instant::now() + Duration::from_secs(secs);
        while Instant::now() < deadline {
            match self.line() {
                Ok(Some(line)) if line["id"] == id => return Some(line),
                Ok(_) => {}
                Err(()) => return None,
            }
        }
        panic!("no answer within {secs} s, nor hung up on");
    }

    fn ask(&mut self, method: &str, params: Value) -> Value {
        let id = self.send(method, params);
        self.answer(id, 30).unwrap_or_else(|| panic!("hung up on, asked {method}"))
    }
}

/// The service of `library`, on a thread: a receiver told when `run`
/// returns.
fn start(root: &Path, socket: &Path, library: &Path, source: Arc<dyn Source>, linger: Duration) -> mpsc::Receiver<String> {
    let config = Config {
        library: library.to_path_buf(),
        cache: root.join("location/cache"),
        location: root.join("location"),
        socket: socket.to_path_buf(),
        source,
        kinds: pinholed::builtin_kinds(PathBuf::from("/nonexistent/pinhole-auth")),
        limit: None,
        settings: root.join("config/pinhole/settings.json"),
        plugins: root.join("config/pinhole/plugins.json"),
        adopt: None,
        downloads: 2,
        sync_every: Some(1),
        given: Some(linger),
    };
    let (done, ended) = mpsc::channel();
    std::thread::spawn(move || {
        let result = service::run(config);
        let _ = done.send(format!("{result:?}"));
    });
    // Up once its socket answers.
    let deadline = Instant::now() + Duration::from_secs(10);
    while UnixStream::connect(socket).is_err() {
        assert!(Instant::now() < deadline, "no service on {}", socket.display());
        std::thread::sleep(Duration::from_millis(20));
    }
    ended
}

fn socket(name: &str) -> PathBuf {
    // Not under TMPDIR, which may be too long a path for a socket.
    let socket = PathBuf::from("/tmp").join(format!("pinhole-ends-{name}-{}.sock", std::process::id()));
    let _ = std::fs::remove_file(&socket);
    socket
}

/// How long after `gone` the service ended, if within `secs`.
fn ended_after(ended: &mpsc::Receiver<String>, gone: Instant, secs: u64) -> Option<Duration> {
    let result = ended.recv_timeout(Duration::from_secs(secs)).ok()?;
    assert_eq!(result, "Ok(())");
    Some(gone.elapsed())
}

/// Files in `dir`, at any depth, whose names say they are being written.
fn parts(dir: &Path) -> Vec<PathBuf> {
    let mut found = Vec::new();
    let Ok(entries) = std::fs::read_dir(dir) else { return found };
    for entry in entries.flatten() {
        let path = entry.path();
        if path.is_dir() {
            found.extend(parts(&path));
        } else if pinhole_core::storage::is_part(&path) {
            found.push(path);
        }
    }
    found
}

/// The processes running `program`'s path, by pid.
fn running(program: &Path) -> Vec<u32> {
    let mut pids = Vec::new();
    for entry in std::fs::read_dir("/proc").unwrap().flatten() {
        let Ok(pid) = entry.file_name().to_string_lossy().parse::<u32>() else { continue };
        let Ok(cmdline) = std::fs::read(entry.path().join("cmdline")) else { continue };
        let stat = std::fs::read_to_string(entry.path().join("stat")).unwrap_or_default();
        let zombie = stat.split(") ").nth(1).is_some_and(|s| s.starts_with('Z'));
        if !zombie && cmdline.split(|b| *b == 0).any(|arg| Path::new(&*String::from_utf8_lossy(arg)) == program) {
            pids.push(pid);
        }
    }
    pids
}

#[test]
fn a_service_of_a_library_given_ends_after_its_windows_whatever_went_on() {
    let root = std::env::temp_dir().join(format!("pinhole-ends-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&root);
    for (var, dir) in [("HOME", "home"), ("XDG_CONFIG_HOME", "config"), ("XDG_DATA_HOME", "data"),
                       ("XDG_STATE_HOME", "state"), ("XDG_CACHE_HOME", "cache")] {
        std::fs::create_dir_all(root.join(dir)).unwrap();
        std::env::set_var(var, root.join(dir));
    }
    let photos = root.join("photos");
    std::fs::create_dir_all(&photos).unwrap();
    image::RgbImage::from_pixel(64, 48, image::Rgb([200, 40, 40])).save(photos.join("a.jpg")).unwrap();
    // A Live Photo, if there is an ffmpeg to make its motion with.
    let ffmpeg = std::process::Command::new("ffmpeg")
        .args(["-v", "error", "-y", "-f", "lavfi", "-i", "color=c=gray:s=160x120:d=1:r=30", "-c:v", "libx264", "-pix_fmt", "yuv420p", "-f", "mov"])
        .arg(photos.join("IMG_1.MOV"))
        .status()
        .is_ok_and(|s| s.success());
    if ffmpeg {
        image::RgbImage::from_pixel(160, 120, image::Rgb([90, 90, 90])).save(photos.join("IMG_1.JPG")).unwrap();
    } else {
        eprintln!("no ffmpeg: a program making something as the service ends is not tried");
    }
    let id = format!("folder:{}", photos.display());
    let entry = json!({"kind": "folder", "path": photos});
    std::fs::create_dir_all(root.join("config/pinhole")).unwrap();
    std::fs::write(root.join("config/pinhole/settings.json"), json!({"sources": [entry], "source": entry}).to_string()).unwrap();
    // A copy of the folder's library, given.
    let given = root.join("given");
    std::fs::create_dir_all(&given).unwrap();
    {
        let mut db = pinhole_core::db::open(&given.join("library.sqlite")).unwrap();
        let source = FolderSource::new(photos.clone());
        pinhole_core::sync::claim(&source, &mut db, &given).unwrap();
        pinhole_core::sync::list(&source, &mut db, &|_, _| {}, &|| false).unwrap();
    }
    let linger = Duration::from_millis(1000);
    let cache = root.join("location/cache");

    // A bug answering a window: that window is hung up on, rather than
    // left waiting, and it no longer counts. Before, its thread ended
    // uncounted and the service never did.
    {
        let socket = socket("local");
        let faulty = Arc::new(Faulty { folder: FolderSource::new(photos.clone()), panics_in: "local" });
        let ended = start(&root, &socket, &given, faulty, linger);
        let mut window = Window::connect(&socket);
        let other = Window::connect(&socket);
        let id_ = window.send("fetch", json!({"asset": "a.jpg", "kind": "original", "source": id}));
        assert_eq!(window.answer(id_, 10), None, "not hung up on after a panic answering it");
        // The other window is served still.
        let mut other = other;
        assert_eq!(other.ask("ping", json!({}))["ok"], true);
        drop(window);
        let gone = Instant::now();
        drop(other);
        let after = ended_after(&ended, gone, 10).expect("still running 10 s after its last window, one of them hung up on after a panic");
        assert!(after >= linger && after < linger + Duration::from_secs(3), "ended {after:?} after its last window");
        assert!(!socket.exists());
    }

    // A bug getting a file (a panic in the source's open): answered with
    // an error, and no longer under way, so the service ends as soon as
    // its windows are gone, not half a minute later.
    {
        let socket = socket("open");
        let faulty = Arc::new(Faulty { folder: FolderSource::new(photos.clone()), panics_in: "open" });
        let ended = start(&root, &socket, &given, faulty, linger);
        let mut window = Window::connect(&socket);
        let medium = window.ask("fetch", json!({"asset": "a.jpg", "kind": "medium", "source": id}));
        assert_eq!(medium["ok"], false, "{medium:#}");
        assert_eq!(medium["error"]["code"], "unavailable", "{medium:#}");
        assert!(medium["error"]["message"].as_str().unwrap().contains("panicked"), "{medium:#}");
        let status = window.ask("status", json!({}));
        assert_eq!(status["result"]["in_flight"], 0, "{status:#}");
        // The downloads go on after it: the same asked again, once the
        // source is right, is got.
        PANICS.store(false, Ordering::SeqCst);
        let medium = window.ask("fetch", json!({"asset": "a.jpg", "kind": "medium", "source": id}));
        assert_eq!(medium["ok"], true, "{medium:#}");
        PANICS.store(true, Ordering::SeqCst);
        let gone = Instant::now();
        drop(window);
        let after = ended_after(&ended, gone, 10).expect("still running 10 s after its last window");
        assert!(after < linger + Duration::from_secs(3), "ended {after:?} after its last window");
        assert!(parts(&cache).is_empty(), "{:?}", parts(&cache));
    }

    // Its socket removed while it runs, and another service started on
    // that path: it still ends after its windows, and leaves the other's
    // socket be.
    {
        let socket = socket("replaced");
        let first = start(&root, &socket, &given, Arc::new(FolderSource::new(photos.clone())), linger);
        let mut window = Window::connect(&socket);
        std::fs::remove_file(&socket).unwrap();
        let second = start(&root, &socket, &given, Arc::new(FolderSource::new(photos.clone())), linger);
        let mut newer = Window::connect(&socket);
        assert_eq!(window.ask("ping", json!({}))["ok"], true);
        let gone = Instant::now();
        drop(window);
        let after = ended_after(&first, gone, 10).expect("still running 10 s after its last window, its socket removed");
        assert!(after < linger + Duration::from_secs(3), "ended {after:?} after its last window");
        assert!(socket.exists(), "it removed the socket of the service started in its place");
        assert_eq!(Window::connect(&socket).ask("ping", json!({}))["ok"], true);
        assert_eq!(newer.ask("ping", json!({}))["ok"], true);
        let gone = Instant::now();
        drop(newer);
        ended_after(&second, gone, 10).expect("the second still running 10 s after its last window");
        assert!(!socket.exists());
    }

    // Making a bounce as its last window goes, with an ffmpeg that takes
    // its time: once the half minute for what is under way is over, it is
    // stopped and its part file gone, none written after; what the service
    // was making thumbnails from goes too.
    if ffmpeg {
        let slow = root.join("slow-bin");
        std::fs::create_dir_all(&slow).unwrap();
        let program = slow.join("ffmpeg");
        std::fs::write(&program, "#!/bin/sh\nfor last; do :; done\necho partial > \"$last\"\nsleep 600\necho more >> \"$last\"\n").unwrap();
        std::fs::set_permissions(&program, std::os::unix::fs::PermissionsExt::from_mode(0o755)).unwrap();
        let path = std::env::var_os("PATH").unwrap_or_default();
        let mut slow_path = std::ffi::OsString::from(&slow);
        slow_path.push(":");
        slow_path.push(&path);
        std::env::set_var("PATH", &slow_path);

        let socket = socket("making");
        let ended = start(&root, &socket, &given, Arc::new(FolderSource::new(photos.clone())), linger);
        let mut window = Window::connect(&socket);
        window.send("fetch", json!({"asset": "IMG_1.JPG", "kind": "bounce", "source": id}));
        let deadline = Instant::now() + Duration::from_secs(10);
        while parts(&cache).is_empty() {
            assert!(Instant::now() < deadline, "the bounce is not being made");
            std::thread::sleep(Duration::from_millis(50));
        }
        assert_eq!(running(&program).len(), 1, "{:?}", running(&program));
        let making = pinhole_core::storage::making();
        std::fs::create_dir_all(&making).unwrap();
        std::fs::write(making.join("original.heic"), b"x").unwrap();
        let gone = Instant::now();
        drop(window);
        let after = ended_after(&ended, gone, 45).expect("still running 45 s after its last window, making something");
        std::env::set_var("PATH", &path);
        assert!(after >= linger + Duration::from_secs(29) && after < linger + Duration::from_secs(33), "ended {after:?} after its last window");
        let deadline = Instant::now() + Duration::from_secs(5);
        while !running(&program).is_empty() {
            assert!(Instant::now() < deadline, "the program making the bounce goes on: {:?}", running(&program));
            std::thread::sleep(Duration::from_millis(50));
        }
        assert!(parts(&cache).is_empty(), "{:?}", parts(&cache));
        assert!(!making.exists(), "{} is left", making.display());
    }

    // The shared service never ends on its own: no window for longer than
    // a given one waits, and it still answers.
    {
        let socket = socket("shared");
        let location = root.join("shared-location");
        let config = Config {
            library: pinhole_core::storage::library_for(&location, &FolderSource::new(photos.clone())),
            cache: location.join("cache"),
            location,
            socket: socket.clone(),
            source: Arc::new(FolderSource::new(photos.clone())),
            kinds: pinholed::builtin_kinds(PathBuf::from("/nonexistent/pinhole-auth")),
            limit: None,
            settings: root.join("config/pinhole/settings.json"),
            plugins: root.join("config/pinhole/plugins.json"),
            adopt: None,
            downloads: 2,
            sync_every: Some(3600),
            given: None,
        };
        let (done, ended) = mpsc::channel();
        std::thread::spawn(move || {
            let result = service::run(config);
            let _ = done.send(format!("{result:?}"));
        });
        let mut window = Window::connect(&socket);
        assert_eq!(window.ask("ping", json!({}))["ok"], true);
        drop(window);
        assert!(ended.recv_timeout(Duration::from_secs(4)).is_err(), "the shared service ended on its own");
        assert_eq!(Window::connect(&socket).ask("ping", json!({}))["ok"], true);
        let _ = std::fs::remove_file(&socket);
    }

    let _ = std::fs::remove_dir_all(&root);
}
