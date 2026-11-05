//! A plugin's workers (docs/plugins.md, Workers; docs/service.md,
//! `workers`): the real service on a socket of its own, a folder as its
//! source, everything it keeps (HOME too) in a scratch folder, and a
//! built-in plugin there whose worker writes down what it is given. It
//! runs for the source shown, the `workers` method and event say so, and
//! it ends as its plugin is turned off.

use std::io::{BufRead, BufReader, Write};
use std::os::unix::fs::PermissionsExt;
use std::os::unix::net::UnixStream;
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::time::{Duration, Instant};

use pinhole_core::plugins::Places;
use pinhole_core::service::{self, Config};
use pinhole_core::source::Source;
use pinhole_folder::FolderSource;
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
        let deadline = Instant::now() + Duration::from_secs(30);
        while Instant::now() < deadline {
            if let Some(line) = self.line() {
                if line["id"] == id {
                    assert_eq!(line["ok"], true, "{method}: {line:#}");
                    return line["result"].clone();
                }
                self.heard.push(line);
            }
        }
        panic!("no answer to {method} within 30 s");
    }

    /// `workers`, asked until `done` says it is so, within a minute.
    fn workers_until(&mut self, done: impl Fn(&Value) -> bool) -> Value {
        let deadline = Instant::now() + Duration::from_secs(60);
        loop {
            let workers = self.ask("workers", json!({}));
            if done(&workers["workers"]) {
                return workers["workers"].clone();
            }
            assert!(Instant::now() < deadline, "not so: {workers:#}");
            std::thread::sleep(Duration::from_millis(200));
        }
    }
}

#[test]
fn a_plugins_worker_runs_for_the_source_shown_while_the_plugin_is_on() {
    let root = std::env::temp_dir().join(format!("pinhole-workers-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&root);
    for (var, dir) in [("HOME", "home"), ("XDG_CONFIG_HOME", "config"), ("XDG_DATA_HOME", "data"),
                       ("XDG_STATE_HOME", "state"), ("XDG_CACHE_HOME", "cache")] {
        std::fs::create_dir_all(root.join(dir)).unwrap();
        std::env::set_var(var, root.join(dir));
    }
    let photos = root.join("photos");
    std::fs::create_dir_all(&photos).unwrap();
    image::RgbImage::from_pixel(64, 48, image::Rgb([200, 40, 40])).save(photos.join("a.jpg")).unwrap();
    let entry = json!({"kind": "folder", "path": photos});
    let id = format!("folder:{}", photos.display());
    let settings = root.join("config/pinhole/settings.json");
    std::fs::create_dir_all(settings.parent().unwrap()).unwrap();
    std::fs::write(&settings, json!({"sources": [entry], "source": entry}).to_string()).unwrap();
    let plugins = root.join("config/pinhole/plugins.json");

    // A built-in plugin with a worker that says what it was given, and
    // that it ended, as its stdin closed.
    let builtin = root.join("builtin");
    let plugin = builtin.join("indexer");
    std::fs::create_dir_all(&plugin).unwrap();
    std::fs::write(plugin.join("Main.qml"), "import Pinhole.Plugin\nPinholePlugin {}\n").unwrap();
    let manifest = json!({"schemaVersion": 1, "api": 10, "id": "test.indexer", "entryPoints": {"main": "Main.qml"},
                          "workers": [{"name": "index", "program": "bin/index", "args": ["daemon"], "per": "source"}]});
    std::fs::write(plugin.join("manifest.json"), manifest.to_string()).unwrap();
    let said = root.join("said");
    std::fs::create_dir_all(plugin.join("bin")).unwrap();
    std::fs::write(plugin.join("bin/index"), format!(
        "#!/bin/sh\necho \"$@\" >> '{0}'\necho \"nice $(cut -d' ' -f19 /proc/$$/stat)\" >> '{0}'\necho working >&2\ncat > /dev/null\necho ended >> '{0}'\n",
        said.display())).unwrap();
    std::fs::set_permissions(plugin.join("bin/index"), std::fs::Permissions::from_mode(0o755)).unwrap();
    let places = Places { builtin, personal: root.join("config/pinhole/plugins"), state: root.join("state/pinhole/sources") };

    let source: Arc<dyn Source> = Arc::new(FolderSource::new(photos.clone()));
    let location = root.join("data/pinhole");
    let library = pinhole_core::storage::library_for(&location, source.as_ref());
    let socket = PathBuf::from("/tmp").join(format!("pinhole-workers-{}.sock", std::process::id()));
    let _ = std::fs::remove_file(&socket);
    let config = Config {
        library: library.clone(),
        cache: location.join("cache"),
        location,
        socket: socket.clone(),
        source,
        kinds: pinholed::builtin_kinds(PathBuf::from("/nonexistent/pinhole-auth")).with_providers(places, Default::default()),
        limit: None,
        settings,
        plugins: plugins.clone(),
        adopt: None,
        downloads: 2,
        sync_every: Some(3600),
        given: None,
    };
    std::thread::spawn(move || service::run(config));
    let mut window = Window::connect(&socket);

    // Once the library is there: running for it, and said to be.
    let workers = window.workers_until(|w| w[0]["running"] == true);
    let one = &workers[0];
    assert_eq!((one["plugin"].as_str(), one["name"].as_str()), (Some("test.indexer"), Some("index")), "{workers:#}");
    assert_eq!(one["key"], json!(id));
    assert_eq!(one["library"], json!(library));
    assert_eq!(one["last_exit"], Value::Null);
    assert_eq!(workers.as_array().unwrap().len(), 1);
    let cache = root.join("cache/pinhole/plugins/test.indexer").join(format!(
        "{}-{}", library.parent().unwrap().file_name().unwrap().to_string_lossy(), library.file_name().unwrap().to_string_lossy()));
    assert!(cache.is_dir(), "{} not made", cache.display());
    let deadline = Instant::now() + Duration::from_secs(10);
    while std::fs::read_to_string(&said).unwrap_or_default().lines().count() < 2 {
        assert!(Instant::now() < deadline, "the worker said nothing");
        std::thread::sleep(Duration::from_millis(50));
    }
    let text = std::fs::read_to_string(&said).unwrap();
    let lines: Vec<&str> = text.lines().collect();
    assert_eq!(lines[0], format!("daemon --library {} --library-key {id} --cache-dir {} --managed-plugins {}",
                                 library.display(), cache.display(), plugins.display()));
    // Ten lower than the service's (this test's) own.
    let own: i32 = std::fs::read_to_string("/proc/self/stat").unwrap().rsplit(") ").next().unwrap().split(' ').nth(16).unwrap().parse().unwrap();
    assert_eq!(lines[1], format!("nice {}", (own + 10).min(19)));
    let event = window.heard.iter().find(|e| e["event"] == "workers" && e["workers"][0]["running"] == true).cloned();
    assert!(event.is_some(), "no workers event: {:#?}", window.heard);

    // Turned off: it ends as its stdin closes, and is no longer listed.
    std::fs::write(&plugins, json!({"enabled": {"test.indexer": false}}).to_string()).unwrap();
    window.workers_until(|w| w.as_array().is_some_and(Vec::is_empty));
    let deadline = Instant::now() + Duration::from_secs(10);
    while !std::fs::read_to_string(&said).unwrap_or_default().contains("ended") {
        assert!(Instant::now() < deadline, "the worker did not end");
        std::thread::sleep(Duration::from_millis(50));
    }
    let _ = std::fs::remove_file(&socket);
    let _ = std::fs::remove_dir_all(&root);
}
