//! A damaged file in a folder (a HEIC or a video cut short, an empty JPEG)
//! is that file's alone: the folder stays fine, its other photos get their
//! thumbnails, the damaged one is not made again every round nor every
//! time a window asks, and once it is replaced by a good one it gets its
//! thumbnail. A folder that cannot be read is still the folder's. The real
//! service on a socket of its own, everything it keeps (and HOME) in a
//! scratch folder.

use std::io::{BufRead, BufReader, Write};
use std::os::unix::fs::PermissionsExt;
use std::os::unix::net::UnixStream;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::sync::Arc;
use std::time::{Duration, Instant};

use pinhole_core::service::{self, Config};
use pinhole_core::source::Source;
use pinhole_folder::FolderSource;
use serde_json::{json, Value};

/// A window's line to the service.
struct Window {
    out: UnixStream,
    lines: BufReader<UnixStream>,
    next: u64,
    /// Events heard so far.
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

    /// A round asked for now, and waited for to its end.
    fn round(&mut self) {
        // Its start (no next round yet), then its end (the next one set):
        // told maybe before the answer to sync is, as that is waited for.
        let before = self.heard.len();
        assert_eq!(self.ask("sync", json!({}))["ok"], true);
        let deadline = Instant::now() + Duration::from_secs(120);
        let mut started = false;
        for line in &self.heard[before..] {
            let rounds = line["event"] == "rounds";
            if rounds && line["next"].is_null() {
                started = true;
            } else if rounds && started {
                return;
            }
        }
        while Instant::now() < deadline {
            if let Some(line) = self.line() {
                let rounds = line["event"] == "rounds";
                if rounds && line["next"].is_null() {
                    started = true;
                } else if rounds && started {
                    self.heard.push(line);
                    return;
                }
                self.heard.push(line);
            }
        }
        panic!("no round within 120 s");
    }

    /// The health states this window has been told of.
    fn healths(&self) -> Vec<String> {
        self.heard.iter().filter(|e| e["event"] == "health").map(|e| e["state"].as_str().unwrap().to_owned()).collect()
    }

    fn thumbs(&mut self) -> (u64, u64) {
        let sources = self.ask("sources", json!({}));
        let made = &sources["result"]["sources"][0];
        (made["count"].as_u64().unwrap_or(0), made["thumbs"].as_u64().unwrap_or(0))
    }
}

fn photo(path: &Path, rgb: [u8; 3]) {
    image::RgbImage::from_pixel(64, 48, image::Rgb(rgb)).save(path).unwrap();
}

/// Each program the service runs, by name, logged: a script first on PATH
/// for each that notes it and runs the real one.
fn log_programs(bin: &Path, log: &Path, programs: &[&str]) {
    let path = std::env::var_os("PATH").unwrap_or_default();
    std::fs::create_dir_all(bin).unwrap();
    for program in programs {
        let Some(real) = std::env::split_paths(&path).map(|dir| dir.join(program)).find(|p| p.is_file()) else { continue };
        let script = bin.join(program);
        std::fs::write(&script, format!("#!/bin/sh\necho {program} >> '{}'\nexec '{}' \"$@\"\n", log.display(), real.display()))
            .unwrap();
        std::fs::set_permissions(&script, std::fs::Permissions::from_mode(0o755)).unwrap();
    }
    let dirs = std::iter::once(bin.to_path_buf()).chain(std::env::split_paths(&path));
    std::env::set_var("PATH", std::env::join_paths(dirs).unwrap());
}

fn ran(log: &Path) -> usize {
    std::fs::read_to_string(log).map_or(0, |s| s.lines().count())
}

/// The first half of a file: cut short.
fn cut(from: &Path, to: &Path) {
    let bytes = std::fs::read(from).unwrap();
    std::fs::write(to, &bytes[..bytes.len() / 2]).unwrap();
}

fn has(program: &str) -> bool {
    Command::new(program).arg("-version").stdout(Stdio::null()).stderr(Stdio::null()).status().is_ok_and(|s| s.success())
}

#[test]
fn a_damaged_file_is_its_own_and_the_folder_stays_fine() {
    if !has("magick") {
        return;
    }
    let root = std::env::temp_dir().join(format!("pinhole-unreadable-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&root);
    for (var, dir) in [("HOME", "home"), ("XDG_CONFIG_HOME", "config"), ("XDG_DATA_HOME", "data"),
                       ("XDG_STATE_HOME", "state"), ("XDG_CACHE_HOME", "cache")] {
        std::fs::create_dir_all(root.join(dir)).unwrap();
        std::env::set_var(var, root.join(dir));
    }
    let (photos, made) = (root.join("photos"), root.join("made"));
    std::fs::create_dir_all(&photos).unwrap();
    std::fs::create_dir_all(&made).unwrap();
    photo(&photos.join("a.jpg"), [200, 40, 40]);
    photo(&photos.join("b.jpg"), [40, 200, 40]);
    std::fs::write(photos.join("empty.jpg"), b"").unwrap();
    let mut damaged = vec!["empty.jpg"];
    // A HEIC cut short, where ImageMagick writes HEIC; noise named one
    // where it does not.
    photo(&made.join("c.jpg"), [40, 40, 200]);
    let heic = Command::new("magick").arg(made.join("c.jpg")).arg(format!("heic:{}", made.join("c.heic").display()))
        .stderr(Stdio::null()).status().is_ok_and(|s| s.success());
    if heic {
        cut(&made.join("c.heic"), &photos.join("cut.heic"));
    } else {
        std::fs::write(photos.join("cut.heic"), b"\0\0\0\x18ftypheic not a picture".repeat(64)).unwrap();
    }
    damaged.push("cut.heic");
    if has("ffmpeg") {
        // An MP4 cut short before its index (at its end): no frame in it.
        let whole = made.join("whole.mp4");
        let ok = Command::new("ffmpeg")
            .args(["-v", "error", "-y", "-f", "lavfi", "-i", "testsrc2=duration=3:size=320x180:rate=10", "-pix_fmt", "yuv420p"])
            .arg(&whole)
            .status()
            .unwrap();
        assert!(ok.success());
        cut(&whole, &photos.join("clip.mp4"));
        damaged.push("clip.mp4");
    }
    let programs = root.join("programs.log");
    log_programs(&root.join("bin"), &programs, &["magick", "ffmpeg"]);
    let entry = json!({"kind": "folder", "path": photos});
    let settings = root.join("config/pinhole/settings.json");
    std::fs::create_dir_all(settings.parent().unwrap()).unwrap();
    std::fs::write(&settings, json!({"sources": [entry], "source": entry}).to_string()).unwrap();

    let source: Arc<dyn Source> = Arc::new(FolderSource::new(photos.clone()));
    let location = root.join("data/pinhole");
    let library = pinhole_core::storage::library_for(&location, source.as_ref());
    let socket = PathBuf::from("/tmp").join(format!("pinhole-unreadable-{}.sock", std::process::id()));
    let _ = std::fs::remove_file(&socket);
    let config = Config {
        library,
        cache: location.join("cache"),
        location,
        socket: socket.clone(),
        source,
        kinds: pinholed::builtin_kinds(PathBuf::from("/nonexistent/pinhole-auth")),
        limit: None,
        settings,
        plugins: root.join("config/pinhole/plugins.json"),
        downloads: 2,
        // Rounds only when asked for (`round`).
        sync_every: Some(3600),
        given: None,
    };
    std::thread::spawn(move || service::run(config));
    let mut window = Window::connect(&socket);

    // The good ones get their thumbnails; the folder is fine.
    let all = 2 + damaged.len() as u64;
    let deadline = Instant::now() + Duration::from_secs(120);
    while window.thumbs() != (all, 2) {
        assert!(Instant::now() < deadline, "no thumbnails: {:?}", window.thumbs());
        std::thread::sleep(Duration::from_millis(200));
    }
    window.round();
    assert_eq!(window.ask("status", json!({}))["result"]["health"], "fine");
    assert_eq!(window.thumbs(), (all, 2));
    let first = ran(&programs);
    assert!(first >= 2 + damaged.len(), "{first} programs run");

    // A medium of a damaged one: said unreadable at once, made by nothing
    // (it was turned down as its thumbnail was), the health as it was.
    for name in &damaged {
        let medium = window.ask("fetch", json!({"asset": name, "kind": "medium"}));
        assert_eq!(medium["error"]["code"], "unreadable", "{name}: {medium:#}");
    }
    let medium = window.ask("fetch", json!({"asset": "a.jpg", "kind": "medium"}));
    assert_eq!(medium["ok"], true, "{medium:#}");
    assert_eq!(ran(&programs), first + 1);

    // A new photo: a round makes its thumbnail, and only its.
    photo(&photos.join("d.jpg"), [200, 200, 40]);
    window.round();
    assert_eq!(window.thumbs(), (all + 1, 3));
    assert_eq!(ran(&programs), first + 2);
    window.round();
    assert_eq!(ran(&programs), first + 2);
    assert_eq!(window.ask("status", json!({}))["result"]["health"], "fine");
    assert!(window.healths().iter().all(|h| h == "fine"), "{:?}", window.healths());

    // A damaged one replaced by a good one gets its thumbnail, and its
    // medium.
    std::thread::sleep(Duration::from_millis(20));
    photo(&made.join("empty.jpg"), [40, 200, 200]);
    std::fs::rename(made.join("empty.jpg"), photos.join("empty.jpg")).unwrap();
    window.round();
    assert_eq!(window.thumbs(), (all + 1, 4));
    let medium = window.ask("fetch", json!({"asset": "empty.jpg", "kind": "medium"}));
    assert_eq!(medium["ok"], true, "{medium:#}");

    // A folder that cannot be read is the folder's: failing, as a window
    // asks of it and as a round looks at it, its photos kept.
    std::fs::set_permissions(&photos, std::fs::Permissions::from_mode(0o000)).unwrap();
    let medium = window.ask("fetch", json!({"asset": "b.jpg", "kind": "medium"}));
    assert_eq!(medium["error"]["code"], "unavailable", "{medium:#}");
    assert_eq!(window.ask("status", json!({}))["result"]["health"], "failing");
    std::fs::set_permissions(&photos, std::fs::Permissions::from_mode(0o755)).unwrap();
    window.round();
    assert_eq!(window.ask("status", json!({}))["result"]["health"], "fine");
    std::fs::set_permissions(&photos, std::fs::Permissions::from_mode(0o000)).unwrap();
    window.round();
    std::fs::set_permissions(&photos, std::fs::Permissions::from_mode(0o755)).unwrap();
    assert_eq!(window.ask("status", json!({}))["result"]["health"], "failing");
    assert_eq!(window.thumbs(), (all + 1, 4));
    // And one gone, offline.
    std::fs::rename(&photos, root.join("gone")).unwrap();
    window.round();
    assert_eq!(window.ask("status", json!({}))["result"]["health"], "offline");
    let _ = std::fs::remove_dir_all(&root);
    let _ = std::fs::remove_file(&socket);
}
