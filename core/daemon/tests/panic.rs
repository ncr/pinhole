//! A sync round that panics is a round that failed: the health says so,
//! and the next round goes on (docs/service.md, Staying in sync). Before,
//! the panic ended the sync thread, and with it every round after, unseen.
//! The source here is a folder whose first listing panics, as a bug in a
//! source would, while the round holds the library's lock.

use std::io::{BufRead, BufReader, Write};
use std::os::unix::net::UnixStream;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant};

use anyhow::Result;
use pinhole_core::db::Index;
use pinhole_folder::FolderSource;
use pinhole_core::kinds::{Failure, Kind, Kinds};
use pinhole_core::service::{self, Config};
use pinhole_core::source::{Body, FetchError, Listing, Source, Stored, Ticket};
use serde_json::{json, Value};

/// Listings asked for, of any of these sources: the first panics.
static LISTINGS: AtomicUsize = AtomicUsize::new(0);

struct Panicky(FolderSource);

impl Source for Panicky {
    fn name(&self) -> String {
        "Panicky".into()
    }
    fn kind(&self) -> &'static str {
        "panicky"
    }
    fn id(&self) -> String {
        format!("panicky:{}", self.0.root().display())
    }
    fn library_name(&self) -> String {
        self.0.library_name()
    }
    fn list(&self, index: &mut Index, progress: &(dyn Fn(u64, u64) + Sync)) -> Result<Listing> {
        if LISTINGS.fetch_add(1, Ordering::SeqCst) == 0 {
            panic!("a source tripped over its own feet");
        }
        self.0.list(index, progress)
    }
    fn changes(&self, index: &mut Index, token: &str) -> Result<Listing> {
        self.0.changes(index, token)
    }
    fn prepare(&self, index: &rusqlite::Connection, wanted: &[Stored]) -> Result<Vec<Result<Ticket, FetchError>>> {
        self.0.prepare(index, wanted)
    }
    fn open(&self, ticket: &Ticket, range: Option<(u64, u64)>) -> Result<Body, FetchError> {
        self.0.open(ticket, range)
    }
}

struct PanickyKind;

impl Kind for PanickyKind {
    fn kind(&self) -> &str {
        "panicky"
    }
    fn plugin(&self) -> &str {
        "test.panicky"
    }
    fn normal(&self, entry: &Value) -> Option<Value> {
        Some(json!({"kind": "panicky", "path": entry["path"].as_str()?}))
    }
    fn id_of(&self, entry: &Value) -> String {
        format!("panicky:{}", entry["path"].as_str().unwrap_or(""))
    }
    fn source(&self, entry: &Value) -> Result<Arc<dyn Source>> {
        Ok(Arc::new(Panicky(FolderSource::new(PathBuf::from(entry["path"].as_str().unwrap_or("/"))))))
    }
    fn describe(&self, entry: &Value) -> Value {
        json!({"path": entry["path"]})
    }
    fn entry_for(&self, _params: &Value) -> Result<Value, Failure> {
        Err(Failure::unsupported("not here"))
    }
}

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

#[test]
fn a_round_that_panics_is_one_that_failed_and_the_next_goes_on() {
    let root = std::env::temp_dir().join(format!("pinhole-panic-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&root);
    let photos = root.join("photos");
    std::fs::create_dir_all(&photos).unwrap();
    image::RgbImage::from_pixel(64, 48, image::Rgb([200, 40, 40])).save(photos.join("a.jpg")).unwrap();
    let entry = json!({"kind": "panicky", "path": photos});
    let settings = root.join("settings.json");
    std::fs::write(&settings, json!({"sources": [entry], "source": entry}).to_string()).unwrap();
    let kinds = Kinds::new(vec![Arc::new(PanickyKind)]);
    let source = kinds.source_for(Some(&entry)).unwrap();
    let location = root.join("data");
    let socket = root.join("pinhole.sock");
    let config = Config {
        library: pinhole_core::storage::library_for(&location, source.as_ref()),
        cache: location.join("cache"),
        location,
        socket: socket.clone(),
        source,
        kinds,
        limit: None,
        settings,
        plugins: root.join("plugins.json"),
        adopt: None,
        downloads: 2,
        sync_every: Some(3600),
        given: None,
    };
    std::thread::spawn(move || service::run(config));
    let mut window = Window::connect(&socket);

    // The first round panics: it failed, and windows hear so.
    let failing = window.event(10, |e| e["event"] == "health");
    assert_eq!(failing["state"], "failing", "{failing:#}");
    window.event(5, |e| e["event"] == "rounds" && !e["next"].is_null());
    assert_eq!(LISTINGS.load(Ordering::SeqCst), 1);

    // The next goes on, the library's lock free again: the photo is in.
    assert_eq!(window.ask("sync", json!({}))["ok"], true);
    let changed = window.event(30, |e| e["event"] == "library_changed" && e["added"] == 1);
    assert_eq!(changed["removed"], 0);
    let fine = window.event(10, |e| e["event"] == "health");
    assert_eq!(fine["state"], "fine");
    assert_eq!(LISTINGS.load(Ordering::SeqCst), 2);
    let sources = window.ask("sources", json!({}));
    assert_eq!(sources["result"]["sources"][0]["count"], 1, "{sources:#}");
    assert_eq!(sources["result"]["sources"][0]["thumbs"], 1, "{sources:#}");

    let _ = std::fs::remove_dir_all(&root);
}
