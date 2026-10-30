//! pinholed as a window meets it: the real service on a socket of its own,
//! with a folder of photos as its source, everything it keeps (and HOME
//! itself) in a scratch folder, so no settings, library or Apple ID of this
//! computer's is touched. docs/service.md is what it is held to.

use std::io::{BufRead, BufReader, Write};
use std::os::unix::net::UnixStream;
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::time::{Duration, Instant};

use pinhole_core::folder::FolderSource;
use pinhole_core::service::{self, Config, PROTOCOL};
use pinhole_core::source::Source;
use serde_json::{json, Value};

/// A window's line to the service: requests out, replies and events in.
struct Window {
    out: UnixStream,
    lines: BufReader<UnixStream>,
    next: u64,
    /// Events heard while waiting for something else.
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
        stream.set_read_timeout(Some(Duration::from_secs(1))).unwrap();
        Window { out: stream.try_clone().unwrap(), lines: BufReader::new(stream), next: 1, heard: Vec::new() }
    }

    /// The next line, or None after a second of nothing.
    fn line(&mut self) -> Option<Value> {
        let mut line = String::new();
        match self.lines.read_line(&mut line) {
            Ok(0) => panic!("the service hung up"),
            Ok(_) => Some(serde_json::from_str(&line).unwrap_or_else(|e| panic!("not JSON: {line:?}: {e}"))),
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

    /// Ask, and wait for the answer to this request (events meanwhile kept).
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

/// The colour in the middle of a picture, give or take JPEG.
fn colour(path: &str) -> [u8; 3] {
    let picture = image::open(path).unwrap_or_else(|e| panic!("{path}: {e}")).to_rgb8();
    let p = picture.get_pixel(picture.width() / 2, picture.height() / 2).0;
    p.map(|c| (c as u32 + 10) as u8 / 20 * 20)
}

fn favourite(library: &Path, id: &str) -> bool {
    let db = rusqlite::Connection::open(library.join("library.sqlite")).unwrap();
    db.query_row("SELECT favorite FROM assets WHERE id = ?1", [id], |r| r.get(0)).unwrap()
}

#[test]
fn a_window_meets_the_service_of_a_folder() {
    let root = std::env::temp_dir().join(format!("pinhole-service-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&root);
    // Whatever looks in HOME or XDG folders (Apple IDs signed in to, the
    // settings) looks in here.
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
    let settings = root.join("config/pinhole/settings.json");
    std::fs::create_dir_all(settings.parent().unwrap()).unwrap();
    std::fs::write(&settings, json!({"sources": [entry], "source": entry}).to_string()).unwrap();

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
        auth: PathBuf::from("/nonexistent/pinhole-auth"),
        limit: None,
        settings,
        adopt: None,
        downloads: 2,
        sync_every: None,
    };
    std::thread::spawn(move || service::run(config));
    let mut window = Window::connect(&socket);

    // First which protocol, then where the photos come from.
    let hello = window.line().expect("a hello");
    assert_eq!(hello["event"], "hello");
    assert_eq!(hello["protocol"], PROTOCOL);
    let shown = window.event(5, |e| e["event"] == "source");
    assert_eq!(shown["kind"], "folder");
    assert_eq!(shown["path"], json!(photos));

    // The library is made from the folder, thumbnails and all (perhaps
    // before this window was there to hear of it).
    let deadline = Instant::now() + Duration::from_secs(60);
    loop {
        let sources = window.ask("sources", json!({}));
        let made = &sources["result"]["sources"][0];
        if made["count"] == 2 && made["thumbs"] == 2 {
            break;
        }
        assert!(Instant::now() < deadline, "no library made: {sources:#}");
        std::thread::sleep(Duration::from_millis(200));
    }

    // A folder's original is its own file, served where it is.
    let original = window.ask("fetch", json!({"asset": "a.jpg", "kind": "original"}));
    assert_eq!(original["ok"], true, "{original:#}");
    assert_eq!(original["result"]["path"], json!(photos.join("a.jpg")));
    assert_eq!(original["result"]["cached"], true);
    // Its medium is made into the cache when first asked for.
    let medium = window.ask("fetch", json!({"asset": "a.jpg", "kind": "medium"}));
    assert_eq!(medium["ok"], true, "{medium:#}");
    assert!(Path::new(medium["result"]["path"].as_str().unwrap()).is_file());
    let missing = window.ask("fetch", json!({"asset": "nope.jpg", "kind": "original"}));
    assert_eq!(missing["error"]["code"], "not_found");
    assert_eq!(window.ask("frobnicate", json!({}))["error"]["code"], "bad_request");

    // A favourite: in the library at once, and every window told.
    let favourite_set = window.ask("set_favorite", json!({"assets": ["a.jpg"], "favorite": true}));
    assert_eq!(favourite_set["result"]["changed"], 1, "{favourite_set:#}");
    assert!(favourite(&library, "a.jpg"));
    window.event(5, |e| e["event"] == "library_changed");
    let again = window.ask("set_favorite", json!({"assets": ["a.jpg"], "favorite": true}));
    assert_eq!(again["result"]["changed"], 0);

    // A file gone from the folder goes from the library at the round a
    // window asks for.
    std::fs::remove_file(photos.join("b.jpg")).unwrap();
    assert_eq!(window.ask("sync", json!({}))["ok"], true);
    let changed = window.event(30, |e| e["event"] == "library_changed" && e["removed"] == 1);
    assert_eq!(changed["added"], 0);
    assert!(favourite(&library, "a.jpg"));

    // Another folder with a photo of the same name, shown just as the
    // first one's medium is being made: that medium is still the first's,
    // and the other's is its own.
    let other = root.join("other");
    std::fs::create_dir_all(&other).unwrap();
    image::RgbImage::from_pixel(80, 60, image::Rgb([40, 40, 200])).save(other.join("a.jpg")).unwrap();
    std::fs::remove_dir_all(root.join("data/pinhole/cache")).ok();
    let id = window.next;
    writeln!(window.out, "{}", json!({"id": id, "method": "fetch", "params": {"asset": "a.jpg", "kind": "medium"}})).unwrap();
    window.next += 1;
    let shown = window.ask("set_source", json!({"kind": "folder", "path": other}));
    assert_eq!(shown["ok"], true, "{shown:#}");
    let deadline = Instant::now() + Duration::from_secs(30);
    let first = loop {
        if let Some(i) = window.heard.iter().position(|l| l["id"] == id) {
            break window.heard.remove(i);
        }
        assert!(Instant::now() < deadline, "no medium");
        if let Some(line) = window.line() {
            window.heard.push(line);
        }
    };
    assert_eq!(first["ok"], true, "{first:#}");
    assert_eq!(colour(first["result"]["path"].as_str().unwrap()), [200, 40, 40]);
    // The other's library made, its medium is its own.
    let deadline = Instant::now() + Duration::from_secs(60);
    let medium = loop {
        let medium = window.ask("fetch", json!({"asset": "a.jpg", "kind": "medium"}));
        if medium["ok"] == true {
            break medium;
        }
        assert!(Instant::now() < deadline, "{medium:#}");
        std::thread::sleep(Duration::from_millis(200));
    };
    assert_eq!(colour(medium["result"]["path"].as_str().unwrap()), [40, 40, 200]);

    let _ = std::fs::remove_dir_all(&root);
}
