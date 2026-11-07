//! A service of a library given to a window (pinhole --library, other than
//! the shared service's): only read and fetched for, never synced, and gone
//! once its windows are. Everything in a scratch folder, HOME too.

use std::io::{BufRead, BufReader, Write};
use std::os::unix::net::UnixStream;
use std::path::{Path, PathBuf};
use std::sync::{mpsc, Arc};
use std::time::{Duration, Instant};

use pinhole_folder::FolderSource;
use pinhole_core::service::{self, Config};
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

    /// Whatever comes for `secs`, kept.
    fn listen(&mut self, secs: f32) {
        let deadline = Instant::now() + Duration::from_secs_f32(secs);
        while Instant::now() < deadline {
            if let Some(line) = self.line() {
                self.heard.push(line);
            }
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

fn count(library: &Path) -> i64 {
    let db = rusqlite::Connection::open(library.join("library.sqlite")).unwrap();
    db.query_row("SELECT count(*) FROM assets", [], |r| r.get(0)).unwrap()
}

/// The service of `library`, on a thread: a receiver told when `run`
/// returns.
fn start(root: &Path, name: &str, library: &Path, source: &Path, linger: Duration) -> (PathBuf, mpsc::Receiver<String>) {
    // Not under TMPDIR, which may be too long a path for a socket.
    let socket = PathBuf::from("/tmp").join(format!("pinhole-given-{name}-{}.sock", std::process::id()));
    let _ = std::fs::remove_file(&socket);
    let config = Config {
        library: library.to_path_buf(),
        cache: root.join("location/cache"),
        location: root.join("location"),
        socket: socket.clone(),
        source: Arc::new(FolderSource::new(source.to_path_buf())),
        kinds: pinholed::builtin_kinds(PathBuf::from("/nonexistent/pinhole-auth")),
        limit: None,
        settings: root.join("config/pinhole/settings.json"),
        plugins: root.join("config/pinhole/plugins.json"),
        downloads: 2,
        sync_every: Some(1),
        given: Some(linger),
    };
    let (done, ended) = mpsc::channel();
    std::thread::spawn(move || {
        let result = service::run(config);
        let _ = done.send(format!("{result:?}"));
    });
    (socket, ended)
}

#[test]
fn a_library_given_is_only_read_and_its_service_ends_with_its_windows() {
    let root = std::env::temp_dir().join(format!("pinhole-given-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&root);
    for (var, dir) in [("HOME", "home"), ("XDG_CONFIG_HOME", "config"), ("XDG_DATA_HOME", "data"),
                       ("XDG_STATE_HOME", "state"), ("XDG_CACHE_HOME", "cache")] {
        std::fs::create_dir_all(root.join(dir)).unwrap();
        std::env::set_var(var, root.join(dir));
    }
    let photos = root.join("photos");
    std::fs::create_dir_all(&photos).unwrap();
    photo(&photos.join("a.jpg"), [200, 40, 40]);
    let entry = json!({"kind": "folder", "path": photos});
    std::fs::create_dir_all(root.join("config/pinhole")).unwrap();
    std::fs::write(root.join("config/pinhole/settings.json"), json!({"sources": [entry], "source": entry}).to_string()).unwrap();

    // A copy of the folder's library, a.jpg in it; then b.jpg in the folder.
    let given = root.join("given");
    std::fs::create_dir_all(&given).unwrap();
    {
        let mut db = pinhole_core::db::open(&given.join("library.sqlite")).unwrap();
        let source = FolderSource::new(photos.clone());
        pinhole_core::sync::claim(&source, &mut db, &given).unwrap();
        pinhole_core::sync::list(&source, &mut db, &|_, _| {}, &|| false).unwrap();
    }
    assert_eq!(count(&given), 1);
    photo(&photos.join("b.jpg"), [40, 200, 40]);
    // Not in WAL mode, as a library copied whole may not be: opened to be
    // written, it would be switched to it.
    {
        let db = rusqlite::Connection::open(given.join("library.sqlite")).unwrap();
        let mode: String = db.query_row("PRAGMA journal_mode = DELETE", [], |r| r.get(0)).unwrap();
        assert_eq!(mode, "delete");
    }
    let file = given.join("library.sqlite");
    let before = (std::fs::read(&file).unwrap(), std::fs::metadata(&file).unwrap().modified().unwrap());

    let (socket, ended) = start(&root, "given", &given, &photos, Duration::from_millis(1500));
    let mut window = Window::connect(&socket);
    // Its photos are fetched as the shared service's are.
    let fetched = window.ask("fetch", json!({"asset": "a.jpg", "kind": "original", "source": format!("folder:{}", photos.display())}));
    assert_eq!(fetched["ok"], true, "{fetched:#}");
    // Its medium is made into the cache, as the source is asked for it:
    // the library itself is only read for that, not written.
    let medium = window.ask("fetch", json!({"asset": "a.jpg", "kind": "medium", "source": format!("folder:{}", photos.display())}));
    assert_eq!(medium["ok"], true, "{medium:#}");
    assert!(Path::new(medium["result"]["path"].as_str().unwrap()).exists(), "{medium:#}");
    // No round, even asked for one (sync rounds are a second apart here):
    // b.jpg never comes, nor news of it.
    window.ask("sync", json!({}));
    window.listen(3.0);
    assert_eq!(count(&given), 1, "the library given was synced");
    assert!(!window.heard.iter().any(|e| e["event"] == "library_changed" || e["event"] == "progress"), "{:#?}", window.heard);
    let rounds = window.heard.iter().find(|e| e["event"] == "rounds").expect("rounds");
    assert!(rounds["source"].is_null() && rounds["next"].is_null(), "{rounds:#}");
    // Nothing changed from it: not the source, nor the storage, nor a
    // favourite.
    for (method, params) in [("set_favorite", json!({"assets": ["a.jpg"], "favorite": true})),
                             ("set_source", json!({"kind": "folder", "path": root.join("home")})),
                             ("set_storage", json!({"limit_bytes": 1u64 << 30}))] {
        let answer = window.ask(method, params);
        assert_eq!(answer["error"]["code"], "read_only", "{method}: {answer:#}");
    }
    // The library file just as it was: no byte of it, nor its time, changed.
    let after = (std::fs::read(&file).unwrap(), std::fs::metadata(&file).unwrap().modified().unwrap());
    assert!(before.0 == after.0, "the library given was written");
    assert_eq!(before.1, after.1, "the library given was written");
    assert!(!given.join("library.sqlite-wal").exists(), "the library given was put in WAL mode");
    let status = window.ask("status", json!({}));
    assert_eq!(status["result"]["windows"], 1, "{status:#}");
    // No plugin's worker runs for it.
    let workers = window.ask("workers", json!({}));
    assert_eq!(workers["result"], json!({"workers": []}), "{workers:#}");

    // A second window, then both gone: the service stays while one is
    // there, and ends a while after the last.
    let second = Window::connect(&socket);
    drop(window);
    std::thread::sleep(Duration::from_millis(2500));
    assert!(ended.try_recv().is_err(), "ended with a window still there");
    let gone = Instant::now();
    drop(second);
    let result = ended.recv_timeout(Duration::from_secs(10)).expect("still running 10 s after its last window");
    assert_eq!(result, "Ok(())");
    let waited = gone.elapsed();
    assert!(waited >= Duration::from_millis(1400) && waited < Duration::from_secs(4), "ended {waited:?} after its last window");
    assert!(!socket.exists(), "its socket is left behind");

    // One no window ever comes to ends as well.
    let (socket, ended) = start(&root, "unused", &given, &photos, Duration::from_millis(500));
    let result = ended.recv_timeout(Duration::from_secs(10)).expect("still running with no window at all");
    assert_eq!(result, "Ok(())");
    assert!(!socket.exists());

    let _ = std::fs::remove_dir_all(&root);
}
