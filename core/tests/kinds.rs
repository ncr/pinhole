//! A source kind follows its plugin's switch (docs/service.md, `kinds`):
//! the real service on a socket of its own, a folder as its source, and
//! everything it keeps (HOME too) in a scratch folder. Turned off, the
//! folder is neither listed nor shown and its library is left exactly as
//! it was; turned on again, it is shown at once, with no listing.

use std::io::{BufRead, BufReader, Write};
use std::os::unix::net::UnixStream;
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::time::{Duration, Instant, SystemTime};

use pinhole_core::folder::FolderSource;
use pinhole_core::kinds::Kinds;
use pinhole_core::service::{self, Config};
use pinhole_core::source::Source;
use serde_json::{json, Value};

struct Window {
    out: UnixStream,
    lines: BufReader<UnixStream>,
    next: u64,
    heard: Vec<Value>,
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

    /// The first event the test is after, within `secs`; the rest are kept.
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
        panic!("no such event within {secs} s; heard {:#?}", self.heard);
    }

    /// Forget what was heard so far, and what comes for a moment more.
    fn quiet(&mut self) {
        while self.line().is_some() {}
        self.heard.clear();
    }

    /// Keep what comes for a moment.
    fn drain(&mut self) {
        while let Some(line) = self.line() {
            self.heard.push(line);
        }
    }

    fn ask(&mut self, method: &str, params: Value) -> Value {
        let id = self.next;
        self.next += 1;
        writeln!(self.out, "{}", json!({"id": id, "method": method, "params": params})).unwrap();
        let deadline = Instant::now() + Duration::from_secs(30);
        while Instant::now() < deadline {
            if let Some(line) = self.line() {
                if line["id"] == id {
                    return line;
                }
                self.heard.push(line);
            }
        }
        panic!("no answer to {method} within 30 s");
    }
}

fn photo(path: &Path, rgb: [u8; 3]) {
    image::RgbImage::from_pixel(64, 48, image::Rgb(rgb)).save(path).unwrap();
}

/// Every file under a folder, with its size and when it was written; not
/// SQLite's shared memory (`-shm`), which whoever reads the index touches
/// (this test too).
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

fn meta(library: &Path, key: &str) -> Option<String> {
    let db = rusqlite::Connection::open_with_flags(library.join("library.sqlite"), rusqlite::OpenFlags::SQLITE_OPEN_READ_ONLY).unwrap();
    db.query_row("SELECT value FROM meta WHERE key = ?1", [key], |r| r.get(0)).ok()
}

#[test]
fn a_kind_turned_off_is_neither_listed_nor_shown_nor_touched() {
    let root = std::env::temp_dir().join(format!("pinhole-kinds-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&root);
    for (var, dir) in [("HOME", "home"), ("XDG_CONFIG_HOME", "config"), ("XDG_DATA_HOME", "data"),
                       ("XDG_STATE_HOME", "state"), ("XDG_CACHE_HOME", "cache")] {
        std::fs::create_dir_all(root.join(dir)).unwrap();
        std::env::set_var(var, root.join(dir));
    }
    let photos = root.join("photos");
    std::fs::create_dir_all(&photos).unwrap();
    photo(&photos.join("a.jpg"), [200, 40, 40]);
    photo(&photos.join("b.jpg"), [40, 200, 40]);
    let entry = json!({"kind": "folder", "path": photos});
    let id = format!("folder:{}", photos.display());
    let settings = root.join("config/pinhole/settings.json");
    std::fs::create_dir_all(settings.parent().unwrap()).unwrap();
    std::fs::write(&settings, json!({"sources": [entry], "source": entry}).to_string()).unwrap();
    let written = std::fs::read(&settings).unwrap();
    let plugins = root.join("config/pinhole/plugins.json");

    let source: Arc<dyn Source> = Arc::new(FolderSource::new(photos.clone()));
    let location = root.join("data/pinhole");
    let library = pinhole_core::storage::library_for(&location, source.as_ref());
    let socket = root.join("pinhole.sock");
    let config = Config {
        library: library.clone(),
        cache: location.join("cache"),
        location,
        socket: socket.clone(),
        source,
        kinds: Kinds::builtin(PathBuf::from("/nonexistent/pinhole-auth")),
        limit: None,
        settings: settings.clone(),
        plugins: plugins.clone(),
        adopt: None,
        downloads: 2,
        // Rounds only when asked for: nothing writes the library unseen.
        sync_every: Some(3600),
        given: None,
    };
    std::thread::spawn(move || service::run(config));
    let mut window = Window::connect(&socket);

    // As a window connects: the source, of its plugin, and the kinds.
    let shown = window.event(5, |e| e["event"] == "source");
    assert_eq!((shown["kind"].as_str(), shown["plugin"].as_str()), (Some("folder"), Some("pinhole.folder")), "{shown:#}");
    assert_eq!(shown["path"], json!(photos));
    let kinds = window.event(5, |e| e["event"] == "kinds");
    assert_eq!(kinds["kinds"][0]["kind"], "icloud");
    assert_eq!(kinds["kinds"][0]["plugin"], "pinhole.icloud");
    assert_eq!(kinds["kinds"][0]["needs_sign_in"], true);
    assert_eq!(kinds["kinds"][1], json!({"kind": "folder", "plugin": "pinhole.folder", "on": true, "needs_sign_in": false,
                                         "accepts": kinds["kinds"][1]["accepts"]}));
    assert!(kinds["kinds"][1]["accepts"].as_array().unwrap().contains(&json!("heic")));
    assert_eq!(window.ask("kinds", json!({}))["result"]["kinds"], kinds["kinds"]);

    // The library made, and its first round over.
    let deadline = Instant::now() + Duration::from_secs(60);
    loop {
        let status = window.ask("status", json!({}));
        let sources = window.ask("sources", json!({}));
        let made = &sources["result"]["sources"][0];
        if made["count"] == 2 && made["thumbs"] == 2 && !status["result"]["rounds"]["next"].is_null() {
            assert_eq!(made["plugin"], "pinhole.folder");
            assert_eq!(made["name"], format!("{}/photos", root.file_name().unwrap().to_string_lossy()));
            assert_eq!(sources["result"]["current"], json!(id));
            break;
        }
        assert!(Instant::now() < deadline, "no library made: {sources:#}");
        std::thread::sleep(Duration::from_millis(200));
    }
    let discovered = window.ask("discover", json!({}));
    assert!(discovered["result"]["kinds"]["folder"]["folders"].is_array(), "{discovered:#}");
    assert!(discovered["result"]["kinds"]["icloud"]["icloudpd"].is_array(), "{discovered:#}");
    assert!(discovered["result"]["folders"].is_array() && discovered["result"]["icloudpd"].is_array(), "{discovered:#}");
    let before = files(&library);
    let (listed_at, token) = (meta(&library, "listed_at"), meta(&library, "change_token"));
    assert!(listed_at.is_some() && token.is_some());
    window.quiet();

    // The folder plugin turned off, as a window does it: written, then
    // plugins_changed. Nothing is listed or shown, and every window hears.
    std::fs::write(&plugins, r#"{"enabled": {"pinhole.folder": false}, "toured": []}"#).unwrap();
    let changed = window.ask("plugins_changed", json!({}));
    assert_eq!(changed["result"], json!({}), "{changed:#}");
    let kinds = window.event(5, |e| e["event"] == "kinds");
    assert_eq!(kinds["kinds"][1]["on"], false);
    assert_eq!(kinds["kinds"][0]["on"], true);
    let shown = window.event(5, |e| e["event"] == "source");
    assert!(shown["kind"].is_null(), "{shown:#}");
    let sources = window.event(5, |e| e["event"] == "sources");
    assert_eq!(sources["sources"], json!([]), "{sources:#}");
    assert!(sources["current"].is_null());
    let status = window.ask("status", json!({}));
    assert!(status["result"]["source"]["kind"].is_null(), "{status:#}");
    assert_eq!(window.ask("kinds", json!({}))["result"]["kinds"][1]["on"], false);
    // Nothing of it can be done, and what could be added has none of it.
    for (method, params) in [
        ("set_source", json!({"kind": "folder", "path": photos})),
        ("set_source", json!({"id": id})),
        ("remove_source", json!({"id": id})),
        ("sign_in", json!({"kind": "folder"})),
    ] {
        let refused = window.ask(method, params);
        assert_eq!(refused["error"]["code"], "turned_off", "{method}: {refused:#}");
    }
    let discovered = window.ask("discover", json!({}));
    assert!(discovered["result"]["kinds"].get("folder").is_none(), "{discovered:#}");
    assert!(discovered["result"].get("folders").is_none(), "{discovered:#}");
    // A round meanwhile (there is nothing to look at: it starts and ends
    // at once, with no source to say) leaves it be too.
    window.quiet();
    assert_eq!(window.ask("sync", json!({}))["ok"], true);
    window.event(5, |e| e["event"] == "rounds" && e["source"].is_null());
    window.event(5, |e| e["event"] == "rounds" && e["source"].is_null());
    std::thread::sleep(Duration::from_millis(500));
    assert_eq!(files(&library), before, "the library of a kind turned off was written");
    assert_eq!((meta(&library, "listed_at"), meta(&library, "change_token")), (listed_at.clone(), token));
    assert_eq!(std::fs::read(&settings).unwrap(), written, "settings.json was written");

    // Turned on again by hand, without a word to the service: the next
    // round sees the file changed, and shows it, with no listing.
    window.quiet();
    std::fs::write(&plugins, r#"{"enabled": {"pinhole.folder": true}}"#).unwrap();
    assert_eq!(window.ask("sync", json!({}))["ok"], true);
    let kinds = window.event(5, |e| e["event"] == "kinds");
    assert_eq!(kinds["kinds"][1]["on"], true);
    let shown = window.event(5, |e| e["event"] == "source");
    assert_eq!(shown["id"], json!(id), "{shown:#}");
    let moved = window.event(5, |e| e["event"] == "library_moved");
    assert_eq!(moved["library"], json!(library));
    let sources = window.event(5, |e| e["event"] == "sources" && e["current"] == json!(id));
    assert_eq!(sources["sources"][0]["count"], 2, "{sources:#}");
    // Its round looks at the folder for changes: not a listing.
    window.event(10, |e| e["event"] == "rounds" && e["source"] == json!(id) && !e["last"].is_null() && !e["next"].is_null());
    assert_eq!(meta(&library, "listed_at"), listed_at);
    assert_eq!(std::fs::read(&settings).unwrap(), written);

    // Signing in is iCloud's when no kind is said: here no provider runs.
    let signing = window.ask("sign_in", json!({"apple_id": "me@example.com", "password": "x"}));
    assert_eq!(signing["error"]["code"], "unavailable", "{signing:#}");
    assert!(!root.join("state/pinhole/auth/me@example.com").exists());
    // A folder is not signed out of.
    assert_eq!(window.ask("sign_out", json!({"id": id}))["error"]["code"], "bad_request");
    // iCloud off: no sign-in, and the folder shown as it was.
    window.quiet();
    std::fs::write(&plugins, r#"{"enabled": {"pinhole.icloud": false}}"#).unwrap();
    assert_eq!(window.ask("plugins_changed", json!({}))["ok"], true);
    let kinds = window.event(5, |e| e["event"] == "kinds");
    assert_eq!((kinds["kinds"][0]["on"].as_bool(), kinds["kinds"][1]["on"].as_bool()), (Some(false), Some(true)));
    let signing = window.ask("sign_in", json!({"apple_id": "me@example.com"}));
    assert_eq!(signing["error"]["code"], "turned_off", "{signing:#}");
    assert_eq!(window.ask("submit_code", json!({"kind": "icloud", "apple_id": "me@example.com", "code": "123456"}))["error"]["code"],
               "turned_off");
    assert_eq!(window.ask("status", json!({}))["result"]["source"]["id"], json!(id));
    assert!(window.heard.iter().all(|e| e["event"] != "source"), "{:#?}", window.heard);
    // Nothing changed, nothing said.
    window.quiet();
    assert_eq!(window.ask("plugins_changed", json!({}))["ok"], true);
    std::thread::sleep(Duration::from_millis(300));
    window.drain();
    assert!(window.heard.iter().all(|e| e["event"] != "kinds" && e["event"] != "sources"), "{:#?}", window.heard);

    let _ = std::fs::remove_dir_all(&root);
}
