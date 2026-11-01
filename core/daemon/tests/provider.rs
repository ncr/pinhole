//! Sources of your own, out of process (docs/source-providers.md), through
//! the test source in tests/provider (a python3 -I script): each test runs
//! pinholed itself, the binary, with everything it keeps (HOME and every
//! XDG folder) in a scratch folder, a socket of its own, and the test
//! source installed as a plugin of your own there, turned on in a scratch
//! plugins.json. It talks to it as a window does.

use std::collections::BTreeSet;
use std::io::{BufRead, BufReader, Write};
use std::os::unix::net::UnixStream;
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::time::{Duration, Instant, SystemTime};

use serde_json::{json, Value};

const KIND: &str = "test.source";

struct Window {
    out: UnixStream,
    lines: BufReader<UnixStream>,
    next: u64,
    heard: Vec<Value>,
}

impl Window {
    fn connect(socket: &Path) -> Window {
        let deadline = Instant::now() + Duration::from_secs(20);
        let stream = loop {
            match UnixStream::connect(socket) {
                Ok(s) => break s,
                Err(_) if Instant::now() < deadline => std::thread::sleep(Duration::from_millis(50)),
                Err(e) => panic!("no service on {}: {e}", socket.display()),
            }
        };
        stream.set_read_timeout(Some(Duration::from_millis(100))).unwrap();
        Window { out: stream.try_clone().unwrap(), lines: BufReader::new(stream), next: 1, heard: Vec::new() }
    }

    fn line(&mut self) -> Option<Value> {
        let mut line = String::new();
        match self.lines.read_line(&mut line) {
            Ok(0) => panic!("the service hung up"),
            Ok(_) => Some(serde_json::from_str(&line).unwrap()),
            Err(_) => None,
        }
    }

    fn ask(&mut self, method: &str, params: Value) -> Value {
        let id = self.next;
        self.next += 1;
        writeln!(self.out, "{}", json!({"id": id, "method": method, "params": params})).unwrap();
        let deadline = Instant::now() + Duration::from_secs(60);
        while Instant::now() < deadline {
            if let Some(line) = self.line() {
                if line["id"] == id {
                    return line;
                }
                self.heard.push(line);
            }
        }
        panic!("no answer to {method} within 60 s");
    }

    fn result(&mut self, method: &str, params: Value) -> Value {
        let answer = self.ask(method, params);
        assert_eq!(answer["ok"], true, "{method}: {answer:#}");
        answer["result"].clone()
    }

    /// The first event the test is after, within `secs`, heard since the
    /// last `forget`.
    fn event(&mut self, secs: u64, wanted: impl Fn(&Value) -> bool) -> Value {
        if let Some(i) = self.heard.iter().position(&wanted) {
            return self.heard.remove(i);
        }
        let deadline = Instant::now() + Duration::from_secs(secs);
        while Instant::now() < deadline {
            if let Some(line) = self.line() {
                if line.get("event").is_some() && wanted(&line) {
                    return line;
                }
                self.heard.push(line);
            }
        }
        panic!("no such event within {secs} s; heard {:#?}", self.heard.iter().rev().take(20).collect::<Vec<_>>());
    }

    fn forget(&mut self) {
        while self.line().is_some() {}
        self.heard.clear();
    }
}

/// A pinholed of the test's own, with everything in a scratch folder.
struct Scratch {
    root: PathBuf,
    socket: PathBuf,
    log: PathBuf,
    plugin: PathBuf,
    pinholed: Option<Child>,
}

impl Scratch {
    fn new(name: &str) -> Scratch {
        let root = std::env::temp_dir().join(format!("pinhole-provider-{name}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        for dir in ["home", "config/pinhole/plugins", "data", "state", "cache", "runtime", "builtin", "photos", "tmp"] {
            std::fs::create_dir_all(root.join(dir)).unwrap();
        }
        // The test source, as a plugin of your own.
        let plugin = root.join("config/pinhole/plugins/test-source");
        std::fs::create_dir_all(&plugin).unwrap();
        let given = Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/provider");
        for file in ["manifest.json", "Main.qml", "pinhole-test-source"] {
            std::fs::copy(given.join(file), plugin.join(file)).unwrap();
        }
        // Not in the scratch folder, which a long TMPDIR puts past the
        // longest path a socket can have (108 bytes).
        let socket = PathBuf::from("/tmp").join(format!("pinhole-{name}-{}.sock", std::process::id()));
        let _ = std::fs::remove_file(&socket);
        let scratch = Scratch {
            socket,
            log: root.join("pinholed.log"),
            plugin,
            root,
            pinholed: None,
        };
        scratch.switch(Some(true));
        scratch
    }

    fn photos(&self) -> PathBuf {
        self.root.join("photos")
    }

    fn state(&self) -> PathBuf {
        self.root.join("state/pinhole/sources/test.source")
    }

    /// The test source's switch in plugins.json; none, and it is off (a
    /// plugin of your own).
    fn switch(&self, on: Option<bool>) {
        let enabled = match on {
            Some(on) => json!({KIND: on}),
            None => json!({}),
        };
        let path = self.root.join("config/pinhole/plugins.json");
        std::fs::write(&path, json!({"enabled": enabled, "toured": []}).to_string()).unwrap();
        // A second's mtime apart from the last, for the rounds that look.
        let later = SystemTime::now() + Duration::from_secs(1);
        std::fs::File::options().write(true).open(&path).unwrap().set_modified(later).unwrap();
    }

    fn start(&mut self) -> Window {
        let log = std::fs::File::create(&self.log).unwrap();
        let r = |dir: &str| self.root.join(dir);
        let child = Command::new(env!("CARGO_BIN_EXE_pinholed"))
            .arg("--socket")
            .arg(&self.socket)
            .arg("--auth")
            .arg("/nonexistent/pinhole-auth")
            .env_clear()
            .env("PATH", std::env::var_os("PATH").unwrap_or_default())
            .env("HOME", r("home"))
            .env("XDG_CONFIG_HOME", r("config"))
            .env("XDG_DATA_HOME", r("data"))
            .env("XDG_STATE_HOME", r("state"))
            .env("XDG_CACHE_HOME", r("cache"))
            .env("XDG_RUNTIME_DIR", r("runtime"))
            // Its own, to see that nothing of it is kept there.
            .env("TMPDIR", r("tmp"))
            .env("PINHOLE_BUILTIN_PLUGINS", r("builtin"))
            .env("PINHOLE_SYNC_SECS", "5")
            .env("PINHOLE_PROVIDER_SILENCE_SECS", "3")
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(log)
            .spawn()
            .unwrap();
        self.pinholed = Some(child);
        let mut window = Window::connect(&self.socket);
        window.event(5, |e| e["event"] == "kinds");
        window
    }

    fn logged(&self) -> String {
        std::fs::read_to_string(&self.log).unwrap_or_default()
    }

    /// The test source's processes running now.
    fn providers(&self) -> Vec<u32> {
        let program = self.plugin.join("pinhole-test-source");
        let program = program.to_string_lossy();
        let mut pids = Vec::new();
        for entry in std::fs::read_dir("/proc").unwrap().flatten() {
            let Ok(pid) = entry.file_name().to_string_lossy().parse::<u32>() else { continue };
            let Ok(cmdline) = std::fs::read(entry.path().join("cmdline")) else { continue };
            if cmdline.split(|b| *b == 0).any(|arg| String::from_utf8_lossy(arg) == program) {
                let stat = std::fs::read_to_string(entry.path().join("stat")).unwrap_or_default();
                // Not a zombie: those have ended, and only wait to be reaped.
                if !stat.split(") ").nth(1).is_some_and(|s| s.starts_with('Z')) {
                    pids.push(pid);
                }
            }
        }
        pids
    }

    fn starts(&self) -> Vec<f64> {
        std::fs::read_to_string(self.state().join("starts.log"))
            .unwrap_or_default()
            .lines()
            .filter_map(|l| l.split(' ').next()?.parse().ok())
            .collect()
    }

    fn misbehave(&self, how: Value) {
        std::fs::write(self.photos().join(".misbehave"), how.to_string()).unwrap();
    }

    fn behave(&self) {
        let _ = std::fs::remove_file(self.photos().join(".misbehave"));
    }
}

impl Drop for Scratch {
    fn drop(&mut self) {
        if let Some(mut child) = self.pinholed.take() {
            let _ = child.kill();
            let _ = child.wait();
        }
        // Its providers end as their stdin closes; any left, killed.
        let deadline = Instant::now() + Duration::from_secs(5);
        while !self.providers().is_empty() && Instant::now() < deadline {
            std::thread::sleep(Duration::from_millis(100));
        }
        for pid in self.providers() {
            let _ = Command::new("kill").arg("-9").arg(pid.to_string()).status();
        }
        let _ = std::fs::remove_file(&self.socket);
        if !std::thread::panicking() {
            let _ = std::fs::remove_dir_all(&self.root);
        } else {
            eprintln!("kept {} (pinholed.log there)", self.root.display());
        }
    }
}

/// The most memory pinholed has had at once, in bytes (VmHWM).
fn peak_memory(pinholed: &Child) -> u64 {
    let status = std::fs::read_to_string(format!("/proc/{}/status", pinholed.id())).unwrap();
    let line = status.lines().find(|l| l.starts_with("VmHWM:")).unwrap();
    line.split_whitespace().nth(1).unwrap().parse::<u64>().unwrap() * 1024
}

fn until(secs: u64, mut done: impl FnMut() -> bool) -> bool {
    let deadline = Instant::now() + Duration::from_secs(secs);
    while Instant::now() < deadline {
        if done() {
            return true;
        }
        std::thread::sleep(Duration::from_millis(100));
    }
    done()
}

fn photo(path: &Path, rgb: [u8; 3]) {
    image::RgbImage::from_pixel(64, 48, image::Rgb(rgb)).save_with_format(path, image::ImageFormat::Jpeg).unwrap();
}

fn photos(dir: &Path, n: usize) {
    for i in 0..n {
        let sub = dir.join(format!("{:02}", i / 100));
        std::fs::create_dir_all(&sub).unwrap();
        photo(&sub.join(format!("IMG_{i:04}.jpg")), [(i * 7 % 256) as u8, (i * 13 % 256) as u8, (i * 29 % 256) as u8]);
    }
}

fn library_of(window: &mut Window) -> PathBuf {
    PathBuf::from(window.result("status", json!({}))["library"].as_str().unwrap())
}

fn db(library: &Path) -> rusqlite::Connection {
    rusqlite::Connection::open_with_flags(library.join("library.sqlite"), rusqlite::OpenFlags::SQLITE_OPEN_READ_ONLY).unwrap()
}

fn count(library: &Path) -> (i64, i64) {
    let Ok(db) = rusqlite::Connection::open_with_flags(library.join("library.sqlite"), rusqlite::OpenFlags::SQLITE_OPEN_READ_ONLY) else {
        return (0, 0);
    };
    db.query_row("SELECT count(*), count(thumb_path) FROM assets", [], |r| Ok((r.get(0)?, r.get(1)?))).unwrap_or((0, 0))
}

/// Every asset with what the window reads of it, and every rendition: what
/// "the library unchanged" means.
fn contents(library: &Path) -> Vec<String> {
    let db = db(library);
    let ok: String = db.query_row("PRAGMA integrity_check", [], |r| r.get(0)).unwrap();
    assert_eq!(ok, "ok");
    let mut rows: Vec<String> = db
        .prepare("SELECT id, favorite, thumb_path, asset_date FROM assets ORDER BY id").unwrap()
        .query_map([], |r| Ok(format!("{:?} {:?} {:?} {:?}", r.get::<_, String>(0)?, r.get::<_, bool>(1)?, r.get::<_, Option<String>>(2)?, r.get::<_, Option<i64>>(3)?)))
        .unwrap()
        .map(|r| r.unwrap())
        .collect();
    rows.extend(
        db.prepare("SELECT asset_id, kind, name, key FROM renditions ORDER BY asset_id, kind").unwrap()
            .query_map([], |r| Ok(format!("{:?} {} {:?} {}", r.get::<_, String>(0)?, r.get::<_, String>(1)?, r.get::<_, String>(2)?, r.get::<_, String>(3)?)))
            .unwrap()
            .map(|r| r.unwrap()),
    );
    rows
}

fn ids(library: &Path) -> BTreeSet<String> {
    db(library).prepare("SELECT id FROM assets").unwrap().query_map([], |r| r.get(0)).unwrap().map(|r| r.unwrap()).collect()
}

fn meta(library: &Path, key: &str) -> Option<String> {
    db(library).query_row("SELECT value FROM meta WHERE key = ?1", [key], |r| r.get(0)).ok()
}

/// Every file under a folder with its size and when it was written; not
/// SQLite's shared memory, which a reader touches.
fn files(dir: &Path) -> Vec<(PathBuf, u64, SystemTime)> {
    let mut out = Vec::new();
    let mut stack = vec![dir.to_owned()];
    while let Some(dir) = stack.pop() {
        for entry in std::fs::read_dir(&dir).unwrap().flatten() {
            let meta = entry.metadata().unwrap();
            if meta.is_dir() {
                stack.push(entry.path());
            } else if !entry.file_name().to_string_lossy().ends_with("-shm") {
                out.push((entry.path(), meta.len(), meta.modified().unwrap()));
            }
        }
    }
    out.sort();
    out
}

/// Signed in to the scratch photos, shown, and listed with every thumbnail.
fn signed_in(scratch: &Scratch, window: &mut Window, tickets: &str, n: i64) -> PathBuf {
    let answer = window.result("sign_in", json!({"kind": KIND, "folder": scratch.photos(), "password": "ok", "tickets": tickets}));
    assert_eq!(answer, json!({"state": "signed_in"}));
    let library = library_of(window);
    assert!(until(60, || count(&library) == (n, n)), "not listed: {:?}\n{}", count(&library), scratch.logged());
    library
}

#[test]
fn a_folder_through_a_provider_listed_with_thumbnails_followed_and_fetched_from() {
    let mut scratch = Scratch::new("list");
    photos(&scratch.photos(), 300);
    let mut window = scratch.start();

    // Its kind, on, with what its manifest says of it.
    let kinds = window.result("kinds", json!({}));
    let kind = kinds["kinds"].as_array().unwrap().iter().find(|k| k["kind"] == KIND).cloned().unwrap();
    assert_eq!(kind, json!({"kind": KIND, "plugin": "test.source", "on": true, "needs_sign_in": true, "accepts": ["jpg", "jpeg"]}));
    // What it finds, under its kind only.
    let found = window.result("discover", json!({}));
    assert_eq!(found["kinds"][KIND], json!({"added": 0}), "{found:#}");
    assert!(found.get("added").is_none(), "{found:#}");

    // Signing in, and its error codes: as the provider says them.
    let folder = scratch.photos();
    let wrong = window.ask("sign_in", json!({"kind": KIND, "folder": folder, "password": "no"}));
    assert_eq!(wrong["error"]["code"], "wrong_password", "{wrong:#}");
    let code = window.result("sign_in", json!({"kind": KIND, "folder": folder, "password": "two-factor", "tickets": "url"}));
    assert_eq!(code, json!({"state": "needs_code"}));
    let wrong = window.ask("submit_code", json!({"kind": KIND, "code": "000000"}));
    assert_eq!(wrong["error"]["code"], "wrong_code", "{wrong:#}");
    // A wrong code is tried again, of the same sign-in.
    assert_eq!(window.result("submit_code", json!({"kind": KIND, "code": "123456"})), json!({"state": "signed_in"}));
    // pinholed never logs what a sign-in is given.
    assert!(!scratch.logged().contains("\"123456\"") && !scratch.logged().contains("two-factor"), "{}", scratch.logged());

    let shown = window.event(10, |e| e["event"] == "source" && e["kind"] == KIND);
    assert_eq!(shown["plugin"], "test.source");
    assert_eq!(shown["name"], "photos");
    let library = library_of(&mut window);
    assert!(library.starts_with(scratch.root.join("data/pinhole/library/test.source")), "{}", library.display());
    assert!(until(60, || count(&library) == (300, 300)), "not listed: {:?}\n{}", count(&library), scratch.logged());
    let sources = window.result("sources", json!({}));
    let source = &sources["sources"][0];
    assert_eq!((source["kind"].as_str(), source["plugin"].as_str(), source["count"].as_i64()), (Some(KIND), Some("test.source"), Some(300)));
    assert_eq!(sources["current"], source["id"]);
    assert!(source["id"].as_str().unwrap().starts_with("test.source:"));
    assert!(meta(&library, "change_token").is_some());

    // The thumbnails came by URL tickets, through its server and the header
    // it gave: each is its photo itself, decoded for its size and colour.
    let (w, h, colour, thumb): (i64, i64, i64, String) = db(&library)
        .query_row("SELECT thumb_w, thumb_h, avg_color, thumb_path FROM assets WHERE id = '00/IMG_0003.jpg'", [], |r| {
            Ok((r.get(0)?, r.get(1)?, r.get(2)?, r.get(3)?))
        })
        .unwrap();
    assert_eq!((w, h), (64, 48));
    assert_ne!(colour, 0);
    assert_eq!(std::fs::read(library.join(&thumb)).unwrap(), std::fs::read(folder.join("00/IMG_0003.jpg")).unwrap());
    // Its keys kept apart from any other source's.
    let key: String = db(&library).query_row("SELECT key FROM renditions WHERE asset_id = '00/IMG_0003.jpg' AND kind = 'thumb'", [], |r| r.get(0)).unwrap();
    assert!(key.len() > 10 && key.as_bytes()[8] == b'~' && key[9..].starts_with('t'), "{key}");

    // The original and full by path tickets, into the cache; the medium
    // made by pinholed from the original.
    for kind in ["original", "full"] {
        let fetched = window.result("fetch", json!({"asset": "02/IMG_0250.jpg", "kind": kind}));
        let path = PathBuf::from(fetched["path"].as_str().unwrap());
        assert!(path.starts_with(scratch.root.join("data/pinhole/cache")), "{fetched:#}");
        assert_eq!(std::fs::read(&path).unwrap(), std::fs::read(folder.join("02/IMG_0250.jpg")).unwrap());
    }
    let medium = window.result("fetch", json!({"asset": "02/IMG_0250.jpg", "kind": "medium"}));
    assert_eq!(medium["type"], "public.jpeg");
    let made = image::open(medium["path"].as_str().unwrap()).unwrap();
    assert_eq!((made.width(), made.height()), (64, 48));
    let gone = window.ask("fetch", json!({"asset": "02/IMG_0250.jpg", "kind": "video"}));
    assert_eq!(gone["error"]["code"], "not_found");

    // It never wrote the library: it was given no path to it, works in a
    // folder of its own (mode 0700), and none of its files is open there.
    let pids = scratch.providers();
    assert_eq!(pids.len(), 1, "one process for the source shown: {pids:?}");
    let cwd = std::fs::read_link(format!("/proc/{}/cwd", pids[0])).unwrap();
    assert_eq!(cwd, scratch.state());
    use std::os::unix::fs::PermissionsExt;
    assert_eq!(std::fs::metadata(scratch.state()).unwrap().permissions().mode() & 0o777, 0o700);
    let cmdline = std::fs::read(format!("/proc/{}/cmdline", pids[0])).unwrap();
    assert!(!String::from_utf8_lossy(&cmdline).contains("data/pinhole"));
    for fd in std::fs::read_dir(format!("/proc/{}/fd", pids[0])).unwrap().flatten() {
        let target = std::fs::read_link(fd.path()).unwrap_or_default();
        assert!(!target.starts_with(scratch.root.join("data")), "{} open by the provider", target.display());
    }

    // Changes: it pushes them, and a round brings them in.
    window.forget();
    let listed_at = meta(&library, "listed_at");
    let before = ids(&library);
    for gone in ["00/IMG_0000.jpg", "00/IMG_0001.jpg", "01/IMG_0150.jpg"] {
        std::fs::remove_file(folder.join(gone)).unwrap();
    }
    photos(&folder.join("new"), 5);
    let edited = folder.join("02/IMG_0299.jpg");
    let old_thumb: String = db(&library).query_row("SELECT thumb_path FROM assets WHERE id = '02/IMG_0299.jpg'", [], |r| r.get(0)).unwrap();
    std::thread::sleep(Duration::from_millis(20));
    photo(&edited, [255, 255, 255]);
    let changed = window.event(15, |e| e["event"] == "library_changed" && e["added"].as_u64() > Some(0));
    assert_eq!((changed["added"].as_u64(), changed["removed"].as_u64()), (Some(5), Some(3)), "{changed:#}");
    assert!(until(10, || count(&library) == (302, 302)), "{:?}", count(&library));
    let after = ids(&library);
    assert!(!after.contains("00/IMG_0000.jpg") && after.contains("new/00/IMG_0004.jpg"));
    assert_eq!(after.len(), before.len() + 2);
    let new_thumb: String = db(&library).query_row("SELECT thumb_path FROM assets WHERE id = '02/IMG_0299.jpg'", [], |r| r.get(0)).unwrap();
    assert_ne!(old_thumb, new_thumb, "an edit has a new thumbnail");
    // No listing for it, but the first: the change feed. Nor was a
    // token said to be refused: there was none before the first.
    assert_eq!(meta(&library, "listed_at"), listed_at);
    assert!(!scratch.logged().contains("no longer accepted"), "{}", scratch.logged());

    // Added through it: copied in by the provider, which says how it went.
    let dropped = scratch.root.join("dropped.jpg");
    photo(&dropped, [1, 2, 3]);
    let added = window.result("add", json!({"files": [dropped]}));
    assert_eq!((added["added"].as_u64(), added["files"][0]["asset"].as_str()), (Some(1), Some("dropped.jpg")), "{added:#}");
    assert!(until(15, || ids(&library).contains("dropped.jpg")));
    let refused = window.result("add", json!({"files": [scratch.root.join("pinholed.log")]}));
    assert_eq!(refused["failed"], 1);

    // Favourites: kept by it, and so in the library.
    assert_eq!(window.result("set_favorite", json!({"assets": ["02/IMG_0200.jpg"], "favorite": true}))["changed"], 1);
    let favourites = std::fs::read_to_string(scratch.state().join(format!("favourites-{}.json", source["id"].as_str().unwrap().trim_start_matches("test.source:")))).unwrap();
    assert!(favourites.contains("02/IMG_0200.jpg"));

    // A second source of the kind, its thumbnails by path tickets; then
    // removed: its library deleted, the provider told.
    let other = scratch.root.join("other");
    photos(&other, 20);
    let answer = window.result("sign_in", json!({"kind": KIND, "folder": other, "password": "ok", "tickets": "path"}));
    assert_eq!(answer["state"], "signed_in");
    let other_library = library_of(&mut window);
    assert_ne!(other_library, library);
    assert!(until(60, || count(&other_library) == (20, 20)), "{:?}\n{}", count(&other_library), scratch.logged());
    let thumb: String = db(&other_library).query_row("SELECT thumb_path FROM assets WHERE id = '00/IMG_0007.jpg'", [], |r| r.get(0)).unwrap();
    assert_eq!(std::fs::read(other_library.join(thumb)).unwrap(), std::fs::read(other.join("00/IMG_0007.jpg")).unwrap());
    let other_id = window.result("status", json!({}))["source"]["id"].as_str().unwrap().to_owned();
    let signed = scratch.state().join("signed-in").join(other_id.trim_start_matches("test.source:"));
    assert!(signed.exists());
    let left = window.result("remove_source", json!({"id": other_id}));
    assert_eq!(left["sources"].as_array().unwrap().len(), 1, "{left:#}");
    assert!(!other_library.exists());
    assert!(until(10, || !signed.exists()), "the provider was not told of the removal");
    assert_eq!(library_of(&mut window), library);
    // One process for the source shown again; the other's stopped.
    assert!(until(10, || scratch.providers().len() == 1), "{:?}", scratch.providers());

    // Signed out: the next round says so.
    window.forget();
    window.result("sign_out", json!({"id": source["id"]}));
    let health = window.event(15, |e| e["event"] == "health" && e["state"] == "needs_sign_in");
    assert_eq!(health["state"], "needs_sign_in");
    assert_eq!(count(&library).0, 303, "the library stays");
}

#[test]
fn a_provider_that_hangs_crashes_floods_or_lies_never_harms_the_library() {
    let mut scratch = Scratch::new("trouble");
    photos(&scratch.photos(), 50);
    let mut window = scratch.start();
    let library = signed_in(&scratch, &mut window, "path", 50);
    let listed = contents(&library);

    // Hangs: stopped after its silence (3 s here), the health failing, and
    // the library as it was.
    window.forget();
    let first = scratch.providers();
    assert_eq!(first.len(), 1);
    scratch.misbehave(json!({"changes": "hang"}));
    window.result("sync", json!({}));
    window.event(15, |e| e["event"] == "health" && e["state"] == "failing");
    assert!(scratch.logged().contains("said nothing for 3 s while asked changes: stopped"), "{}", scratch.logged());
    assert!(until(5, || !scratch.providers().contains(&first[0])), "the hung one still runs");
    assert_eq!(contents(&library), listed);
    scratch.behave();
    window.event(30, |e| e["event"] == "health" && e["state"] == "fine");

    // Crashes: started again at most once in 10 s, the health failing
    // meanwhile.
    window.forget();
    let started = scratch.starts().len();
    scratch.misbehave(json!({"start": "crash"}));
    window.result("sync", json!({}));
    window.event(15, |e| e["event"] == "health" && e["state"] == "failing");
    std::thread::sleep(Duration::from_secs(25));
    let starts = scratch.starts();
    let crashing = &starts[started..];
    assert!((2..=3).contains(&crashing.len()), "started {} times in 25 s: {crashing:?}", crashing.len());
    for pair in crashing.windows(2) {
        assert!(pair[1] - pair[0] >= 9.9, "started again after {:.1} s", pair[1] - pair[0]);
    }
    assert!(scratch.logged().contains("is started again at most once in 10 s"), "{}", scratch.logged());
    assert_eq!(window.result("status", json!({}))["health"], "failing");
    assert_eq!(contents(&library), listed);
    scratch.behave();
    window.event(30, |e| e["event"] == "health" && e["state"] == "fine");

    // Lines that are not the protocol, one too long, a put too big: it is
    // stopped, or what it was asked fails, and nothing of it is written.
    for (i, (how, logged)) in [
        ("garbage", "sent a line that is not a JSON object (\"this is not JSON\"): stopped"),
        ("long_line", "sent a line of more than 4 MB: stopped"),
        ("big_batch", "a put of 1001 assets, more than 1000"),
    ]
    .into_iter()
    .enumerate()
    {
        let listed = contents(&library);
        window.forget();
        scratch.misbehave(json!({"changes": how}));
        // A file changed, so a round would write something if it could.
        photo(&scratch.photos().join("00/IMG_0010.jpg"), [9, 9, i as u8]);
        window.result("sync", json!({}));
        window.event(20, |e| e["event"] == "health" && e["state"] == "failing");
        assert!(scratch.logged().contains(logged), "{how}: {}", scratch.logged());
        assert_eq!(contents(&library), listed, "{how}");
        // Well again, the change comes in.
        scratch.behave();
        window.event(30, |e| e["event"] == "health" && e["state"] == "fine");
        assert!(until(15, || contents(&library) != listed), "{how}: the change never came");
    }

    // Hostile, and broken, listings: lines for other requests, news that is
    // no news, a removal in a listing, and assets that are not right. Only
    // what is right is written; what it had of one it now says wrong stays.
    let reset = || {
        // No token it knows: the next round lists the whole library.
        let _ = std::fs::remove_dir_all(scratch.state().join("seen"));
    };
    window.forget();
    reset();
    scratch.misbehave(json!({"list": "hostile"}));
    window.result("sync", json!({}));
    assert!(until(20, || scratch.logged().matches("listing the whole library").count() >= 1), "{}", scratch.logged());
    assert!(until(20, || ids(&library).len() == 52), "{:?}", ids(&library).len());
    let now = ids(&library);
    let photos_there: BTreeSet<String> = (0..50).map(|i| format!("00/IMG_{i:04}.jpg")).collect();
    assert!(photos_there.is_subset(&now), "the removal in a listing was heard");
    // Odd ids and keys that are right are written as they are, and go
    // nowhere they should not: a key's file is in thumbs/, whatever it says.
    assert!(now.contains("x'); DROP TABLE assets; --"), "an odd id that is right is an id");
    assert!(now.contains("../../../etc/passwd"));
    assert!(until(20, || count(&library) == (52, 52)), "{:?}", count(&library));
    let thumb: String = db(&library).query_row("SELECT thumb_path FROM assets WHERE id = '../../../etc/passwd'", [], |r| r.get(0)).unwrap();
    assert!(thumb.starts_with("thumbs/") && !thumb[7..].contains("../") && thumb.matches('/').count() == 2, "{thumb}");
    assert!(library.join(&thumb).is_file());
    for refused in ["from-another-request", "from-no-request", "bad-kind", "two-thumbs", "no-original", "jpeg-kind", "nul\u{0}id"] {
        assert!(!now.contains(refused), "{refused} written");
    }
    let logged = scratch.logged();
    assert!(logged.contains("test.source: list: bad-kind: kind is not photo, video or live"), "{logged}");
    assert!(logged.contains("test.source: list: two-thumbs: two thumb renditions"), "{logged}");
    assert!(logged.contains("more refused"), "{logged}");
    let ok: String = db(&library).query_row("PRAGMA integrity_check", [], |r| r.get(0)).unwrap();
    assert_eq!(ok, "ok");

    window.forget();
    reset();
    let thumb_before: String = db(&library).query_row("SELECT thumb_path FROM assets WHERE id = '00/IMG_0000.jpg'", [], |r| r.get(0)).unwrap();
    scratch.misbehave(json!({"list": "bad_assets"}));
    window.result("sync", json!({}));
    assert!(until(20, || scratch.logged().matches("listing the whole library").count() >= 2));
    assert!(until(20, || !ids(&library).contains("x'); DROP TABLE assets; --")), "a listing takes away what is gone");
    let now = ids(&library);
    assert_eq!(now, photos_there, "the broken one stays, the rest as listed");
    let thumb_after: String = db(&library).query_row("SELECT thumb_path FROM assets WHERE id = '00/IMG_0000.jpg'", [], |r| r.get(0)).unwrap();
    assert_eq!(thumb_before, thumb_after);

    // A line of its log of 200 MB: its start logged, the rest passed over
    // as it comes, never kept.
    window.forget();
    reset();
    let peak = peak_memory(scratch.pinholed.as_ref().unwrap());
    scratch.misbehave(json!({"list": "huge_stderr"}));
    window.result("sync", json!({}));
    assert!(until(60, || scratch.logged().contains("test.source: after the long line")), "{}", scratch.logged().len());
    assert!(scratch.logged().contains(&format!("test.source: {}…\n", "z".repeat(2000))));
    let grown = peak_memory(scratch.pinholed.as_ref().unwrap()).saturating_sub(peak);
    assert!(grown < 32 << 20, "pinholed grew by {} MB", grown >> 20);
    scratch.behave();

    // Its log into pinholed's, but not without end.
    window.forget();
    reset();
    scratch.misbehave(json!({"list": "spam_stderr"}));
    window.result("sync", json!({}));
    assert!(until(30, || scratch.logged().contains("more than 200 lines of its log in 10 s")), "{}", scratch.logged().len());
    let noise = scratch.logged().matches("test.source: noise").count();
    assert!((150..=400).contains(&noise), "{noise} lines of noise logged");
    scratch.behave();
}

#[test]
fn a_kind_outside_its_plugins_name_is_refused_and_a_plugin_turned_off_stops_its_provider() {
    let mut scratch = Scratch::new("off");
    photos(&scratch.photos(), 30);
    // Plugins that claim kinds not theirs.
    let plugins = scratch.root.join("config/pinhole/plugins");
    for (folder, id, kind) in [("thief", "other.thief", "test.source.x"), ("icloud", "icloud", "icloud"), ("loose", "loose", "folder.loose")] {
        let dir = plugins.join(folder);
        std::fs::create_dir_all(&dir).unwrap();
        for file in ["Main.qml", "pinhole-test-source"] {
            std::fs::copy(scratch.plugin.join(file), dir.join(file)).unwrap();
        }
        let manifest = json!({"schemaVersion": 1, "api": 3, "id": id, "entryPoints": {"main": "Main.qml"},
                              "sources": [{"kind": kind, "provider": "pinhole-test-source"}]});
        std::fs::write(dir.join("manifest.json"), manifest.to_string()).unwrap();
    }
    let path = scratch.root.join("config/pinhole/plugins.json");
    std::fs::write(&path, json!({"enabled": {KIND: true, "other.thief": true, "icloud": true, "loose": true}}).to_string()).unwrap();
    let mut window = scratch.start();
    let kinds = window.result("kinds", json!({}));
    let names: Vec<&str> = kinds["kinds"].as_array().unwrap().iter().filter_map(|k| k["kind"].as_str()).collect();
    assert_eq!(names, ["icloud", "folder", KIND], "{kinds:#}");
    let logged = scratch.logged();
    assert!(logged.contains("a source refused: other.thief: source \"test.source.x\": its kind must be the plugin's id, other.thief"), "{logged}");
    assert!(logged.contains("a source refused: icloud: source \"icloud\": that kind is pinholed's own"), "{logged}");
    assert!(logged.contains("a source refused: loose: source \"folder.loose\": its kind must be"), "{logged}");
    let refused = window.ask("set_source", json!({"kind": "test.source.x", "folder": scratch.photos()}));
    assert_eq!(refused["error"]["code"], "bad_request", "{refused:#}");

    let library = signed_in(&scratch, &mut window, "url", 30);
    let listed_at = meta(&library, "listed_at");
    let id = window.result("status", json!({}))["source"]["id"].as_str().unwrap().to_owned();
    assert!(until(5, || scratch.providers().len() == 1));

    // Turned off: not listed, not shown, its provider stopped, its library
    // and its state left as they were (from the moment it is off: no round
    // of it runs after plugins_changed has answered).
    window.forget();
    scratch.switch(Some(false));
    window.result("plugins_changed", json!({}));
    let before = files(&library);
    let state = files(&scratch.state());
    let kinds = window.event(5, |e| e["event"] == "kinds");
    let kind = kinds["kinds"].as_array().unwrap().iter().find(|k| k["kind"] == KIND).cloned().unwrap();
    assert_eq!(kind["on"], false);
    let sources = window.result("sources", json!({}));
    assert_eq!(sources, json!({"sources": [], "current": null}));
    assert!(until(10, || scratch.providers().is_empty()), "still running: {:?}", scratch.providers());
    assert!(scratch.logged().contains("test.source: turned off: its providers stopped"));
    let off = window.ask("set_source", json!({"id": id}));
    assert_eq!(off["error"]["code"], "turned_off", "{off:#}");
    let off = window.ask("sign_in", json!({"kind": KIND, "folder": scratch.photos(), "password": "ok"}));
    assert_eq!(off["error"]["code"], "turned_off", "{off:#}");
    // Rounds go on meanwhile; nothing starts it.
    std::thread::sleep(Duration::from_secs(7));
    assert!(scratch.providers().is_empty());
    assert_eq!(files(&library), before, "the library of a source turned off was touched");
    let mut state_now = files(&scratch.state());
    state_now.retain(|(p, ..)| !p.ends_with("starts.log"));
    let mut state_then = state.clone();
    state_then.retain(|(p, ..)| !p.ends_with("starts.log"));
    assert_eq!(state_now, state_then);

    // No switch at all: a plugin of your own is off.
    scratch.switch(None);
    window.result("plugins_changed", json!({}));
    assert_eq!(window.result("sources", json!({}))["current"], Value::Null);

    // On again: shown at once, as it was, with no listing.
    window.forget();
    scratch.switch(Some(true));
    window.result("plugins_changed", json!({}));
    let shown = window.event(10, |e| e["event"] == "source");
    assert_eq!(shown["id"], id.as_str());
    assert!(until(15, || window.result("status", json!({}))["rounds"]["last"].is_u64()));
    assert_eq!(meta(&library, "listed_at"), listed_at);
    assert_eq!(count(&library), (30, 30));
    assert!(until(10, || scratch.providers().len() == 1));

    // Its plugin gone altogether: its source stays among the sources, kept
    // for when it is back.
    std::fs::rename(scratch.plugin.clone(), scratch.root.join("away")).unwrap();
    scratch.switch(Some(true));
    window.result("plugins_changed", json!({}));
    assert!(until(10, || scratch.providers().is_empty()));
    assert_eq!(window.result("sources", json!({}))["sources"], json!([]));
    let settings: Value = serde_json::from_str(&std::fs::read_to_string(scratch.root.join("config/pinhole/settings.json")).unwrap()).unwrap();
    assert_eq!(settings["sources"][0]["kind"], KIND, "{settings:#}");
    std::fs::rename(scratch.root.join("away"), scratch.plugin.clone()).unwrap();
    scratch.switch(Some(true));
    window.result("plugins_changed", json!({}));
    let shown = window.event(10, |e| e["event"] == "source" && e["kind"] == KIND);
    assert_eq!(shown["id"], id.as_str());
    assert_eq!(meta(&library, "listed_at"), listed_at);
}

/// Ask for a fetch without waiting for its answer: its id.
fn send_fetch(window: &mut Window, asset: &str, kind: &str) -> u64 {
    let id = window.next;
    window.next += 1;
    writeln!(window.out, "{}", json!({"id": id, "method": "fetch", "params": {"asset": asset, "kind": kind}})).unwrap();
    id
}

/// The answer to request `id`, within `secs`.
fn answer_to(window: &mut Window, id: u64, secs: u64) -> Value {
    let deadline = Instant::now() + Duration::from_secs(secs);
    loop {
        if let Some(i) = window.heard.iter().position(|l| l["id"] == id) {
            return window.heard.remove(i);
        }
        assert!(Instant::now() < deadline, "no answer to {id} within {secs} s");
        if let Some(line) = window.line() {
            window.heard.push(line);
        }
    }
}

#[test]
fn a_providers_files_and_urls_hold_up_no_download_and_are_never_kept_in_memory() {
    let has_ffmpeg = Command::new("ffmpeg").arg("-version").stdout(Stdio::null()).status().is_ok_and(|s| s.success());
    let mut scratch = Scratch::new("files");
    photos(&scratch.photos(), 20);
    // A video, its index first, keyframes every second: one ffmpeg reads
    // by ranges, a few frames of it.
    let video = scratch.photos().join("v/clip.mp4");
    if has_ffmpeg {
        std::fs::create_dir_all(video.parent().unwrap()).unwrap();
        let made = Command::new("ffmpeg")
            .args(["-v", "error", "-f", "lavfi", "-i", "testsrc2=duration=20:size=640x360:rate=25", "-c:v", "libx264", "-preset", "ultrafast",
                   "-b:v", "4M", "-g", "25", "-pix_fmt", "yuv420p", "-movflags", "+faststart"])
            .arg(&video)
            .status()
            .unwrap();
        assert!(made.success());
    }
    let n = 20 + has_ffmpeg as i64;
    let mut window = scratch.start();
    let answer = window.result("sign_in", json!({"kind": KIND, "folder": scratch.photos(), "password": "ok", "tickets": "url", "all_url": true}));
    assert_eq!(answer["state"], "signed_in");
    let library = library_of(&mut window);
    assert!(until(60, || count(&library) == (n, n)), "not listed: {:?}\n{}", count(&library), scratch.logged());
    let served = || std::fs::read_to_string(scratch.state().join("served.log")).unwrap_or_default();

    // The video's thumbnail made by ffmpeg from its URL, read by ranges
    // through pinholed (the ticket's header never on a command line), not
    // downloaded whole first.
    if has_ffmpeg {
        let thumb: String = db(&library).query_row("SELECT thumb_path FROM assets WHERE id = 'v/clip.mp4'", [], |r| r.get(0)).unwrap();
        let made = image::open(library.join(thumb)).unwrap();
        assert!(made.width() >= 320, "{}", made.width());
        let served = served();
        let asked: Vec<&str> = served.lines().filter_map(|l| l.strip_prefix("v/clip.mp4 ")).collect();
        assert!(asked.iter().all(|r| r.starts_with("bytes=")), "{asked:?}");
        assert!(asked.iter().any(|r| *r != "bytes=0-"), "no seek by ranges: {asked:?}");
    }

    // A photo's medium made from its original by URL: fetched into the
    // cache's making/, never into TMPDIR, and gone once made.
    let making = scratch.root.join("data/pinhole/cache/making");
    let tmp = scratch.root.join("tmp");
    let stall = scratch.photos().join(".stall");
    std::fs::write(&stall, b"").unwrap();
    let asked = send_fetch(&mut window, "00/IMG_0005.jpg", "medium");
    let part = || std::fs::read_dir(&making).into_iter().flatten().flatten().any(|e| e.file_name().to_string_lossy().ends_with(".part"));
    let in_tmp = || std::fs::read_dir(&tmp).unwrap().flatten().map(|e| e.file_name()).collect::<Vec<_>>();
    assert!(until(20, part), "no part in {}; in TMPDIR: {:?}", making.display(), in_tmp());
    assert_eq!(in_tmp(), Vec::<std::ffi::OsString>::new(), "pinholed wrote in TMPDIR");
    std::fs::remove_file(&stall).unwrap();
    let medium = answer_to(&mut window, asked, 20);
    assert_eq!(medium["ok"], true, "{medium:#}");
    let made = image::open(medium["result"]["path"].as_str().unwrap()).unwrap();
    assert_eq!((made.width(), made.height()), (64, 48));
    assert!(!part(), "a part left in making/");

    // Originals that are FIFOs (or a mount that no longer answers): more
    // of them than there are downloads, each refused at once, and the
    // downloads free for what comes next.
    window.forget();
    scratch.misbehave(json!({"prepare": "fifo"}));
    let started = Instant::now();
    let asked: Vec<u64> = (0..12).map(|i| send_fetch(&mut window, &format!("00/IMG_{i:04}.jpg"), "original")).collect();
    for id in asked {
        let answer = answer_to(&mut window, id, 20);
        assert_eq!(answer["error"]["code"], "not_found", "{answer:#}");
        assert!(answer["error"]["message"].as_str().unwrap().contains("is not a file"), "{answer:#}");
    }
    assert!(started.elapsed() < Duration::from_secs(10), "{:?}", started.elapsed());
    scratch.behave();
    let fetched = window.result("fetch", json!({"asset": "00/IMG_0015.jpg", "kind": "original"}));
    assert_eq!(std::fs::read(fetched["path"].as_str().unwrap()).unwrap(), std::fs::read(scratch.photos().join("00/IMG_0015.jpg")).unwrap());
}

/// Throughput: 50,000 made-up assets through the protocol, checked and
/// written. Printed, for docs/source-providers.md.
#[test]
fn fifty_thousand_assets_through_the_protocol() {
    let n: i64 = std::env::var("PINHOLE_PROVIDER_ASSETS").ok().and_then(|n| n.parse().ok()).unwrap_or(50_000);
    let mut scratch = Scratch::new("many");
    let mut window = scratch.start();
    let started = Instant::now();
    let answer = window.result("sign_in", json!({"kind": KIND, "folder": scratch.photos(), "password": "ok", "fake": n}));
    assert_eq!(answer["state"], "signed_in");
    let library = library_of(&mut window);
    // While it lists: none of its files is in the library's folder.
    let mut looked = 0;
    let done = until(300, || {
        for pid in scratch.providers() {
            for fd in std::fs::read_dir(format!("/proc/{pid}/fd")).into_iter().flatten().flatten() {
                let target = std::fs::read_link(fd.path()).unwrap_or_default();
                assert!(!target.starts_with(&library), "{} open by the provider", target.display());
                looked += 1;
            }
        }
        count(&library).0 == n && window.result("status", json!({}))["progress"].is_null()
    });
    let took = started.elapsed();
    assert!(done, "{:?} of {n}\n{}", count(&library), scratch.logged());
    assert!(looked > 0);
    eprintln!(
        "{n} assets listed through the protocol in {:.2} s: {:.0} a second ({} kB of library)",
        took.as_secs_f64(),
        n as f64 / took.as_secs_f64(),
        std::fs::metadata(library.join("library.sqlite")).map(|m| m.len() / 1024).unwrap_or(0)
    );
    // It gives no change token: listed again each round, with nothing
    // said of a token that never was.
    let listed_at = meta(&library, "listed_at");
    assert!(until(60, || meta(&library, "listed_at") != listed_at), "not listed again");
    assert!(!scratch.logged().contains("change token"), "{}", scratch.logged());
}
