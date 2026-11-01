//! pinholed as a window meets it: the real service on a socket of its own,
//! with a folder of photos as its source, everything it keeps (and HOME
//! itself) in a scratch folder, so no settings, library or Apple ID of this
//! computer's is touched. docs/service.md is what it is held to.

use std::io::{BufRead, BufReader, Write};
use std::os::unix::net::UnixStream;
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::time::{Duration, Instant};

use pinhole_folder::FolderSource;
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

/// A HEIC with an EXIF date, as an iPhone's has one: a JPEG given the
/// date by hand (an APP1 segment), then turned into a HEIC by ImageMagick,
/// which carries it over. False where ImageMagick writes no HEIC.
fn heic(path: &Path, rgb: [u8; 3], taken: &str) -> bool {
    let jpeg = path.with_extension("source.jpg");
    photo(&jpeg, rgb);
    // TIFF, little-endian: IFD0 pointing to the Exif IFD (at 26), which
    // holds DateTimeOriginal (0x9003), ASCII, 20 bytes (at 44).
    let mut exif = b"Exif\0\0II*\0".to_vec();
    for bytes in [&8u32.to_le_bytes()[..], &1u16.to_le_bytes(), &0x8769u16.to_le_bytes(), &4u16.to_le_bytes(),
                  &1u32.to_le_bytes(), &26u32.to_le_bytes(), &0u32.to_le_bytes(), &1u16.to_le_bytes(),
                  &0x9003u16.to_le_bytes(), &2u16.to_le_bytes(), &20u32.to_le_bytes(), &44u32.to_le_bytes(),
                  &0u32.to_le_bytes(), taken.as_bytes(), b"\0"] {
        exif.extend_from_slice(bytes);
    }
    let plain = std::fs::read(&jpeg).unwrap();
    let mut dated = plain[..2].to_vec();
    dated.extend_from_slice(&[0xFF, 0xE1]);
    dated.extend_from_slice(&(exif.len() as u16 + 2).to_be_bytes());
    dated.extend_from_slice(&exif);
    dated.extend_from_slice(&plain[2..]);
    std::fs::write(&jpeg, dated).unwrap();
    let made = std::process::Command::new("magick")
        .arg(&jpeg)
        .args(["-depth", "8"])
        .arg(format!("heic:{}", path.display()))
        .stderr(std::process::Stdio::null())
        .status()
        .is_ok_and(|s| s.success());
    std::fs::remove_file(&jpeg).unwrap();
    made && path.exists()
}

fn identify(path: &str, format: &str) -> String {
    let out = std::process::Command::new("magick").args(["identify", "-format", format, path]).output().unwrap();
    String::from_utf8_lossy(&out.stdout).into_owned()
}

/// Every ImageMagick the service runs, logged: a `magick` first on PATH
/// that notes its arguments and runs the real one.
fn count_magicks(bin: &Path, log: &Path) {
    let path = std::env::var_os("PATH").unwrap_or_default();
    let Some(real) = std::env::split_paths(&path).map(|dir| dir.join("magick")).find(|p| p.is_file()) else { return };
    std::fs::create_dir_all(bin).unwrap();
    let script = bin.join("magick");
    std::fs::write(&script, format!("#!/bin/sh\necho \"$*\" >> '{}'\nexec '{}' \"$@\"\n", log.display(), real.display())).unwrap();
    std::fs::set_permissions(&script, std::os::unix::fs::PermissionsExt::from_mode(0o755)).unwrap();
    let dirs = std::iter::once(bin.to_path_buf()).chain(std::env::split_paths(&path));
    std::env::set_var("PATH", std::env::join_paths(dirs).unwrap());
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
    let has_heic = heic(&photos.join("c.heic"), [40, 40, 200], "2024:05:06 07:08:09");
    let magicks = root.join("magicks.log");
    count_magicks(&root.join("bin"), &magicks);
    // JPEGs a window made itself: one unused for two days goes as the
    // service starts; one made a moment ago, perhaps on its way somewhere
    // as it starts, stays, and the folder the window writes in with it.
    let compatible = root.join("cache/pinhole/compatible");
    std::fs::create_dir_all(&compatible).unwrap();
    std::fs::write(compatible.join("IMG_0001.jpg"), b"old").unwrap();
    let two_days_ago = std::time::SystemTime::now() - Duration::from_secs(2 * 24 * 3600);
    std::fs::File::options().write(true).open(compatible.join("IMG_0001.jpg")).unwrap().set_modified(two_days_ago).unwrap();
    std::fs::write(compatible.join("IMG_0002.jpg"), b"new").unwrap();
    let entry = json!({"kind": "folder", "path": photos});
    let settings = root.join("config/pinhole/settings.json");
    std::fs::create_dir_all(settings.parent().unwrap()).unwrap();
    std::fs::write(&settings, json!({"sources": [entry], "source": entry}).to_string()).unwrap();

    let source: Arc<dyn Source> = Arc::new(FolderSource::new(photos.clone()));
    let location = root.join("data/pinhole");
    let library = pinhole_core::storage::library_for(&location, source.as_ref());
    // Not under TMPDIR, which may be too long a path for a socket.
    let socket = PathBuf::from("/tmp").join(format!("pinhole-service-{}.sock", std::process::id()));
    let _ = std::fs::remove_file(&socket);
    let config = Config {
        library: library.clone(),
        cache: location.join("cache"),
        location,
        socket: socket.clone(),
        source,
        kinds: pinholed::builtin_kinds(PathBuf::from("/nonexistent/pinhole-auth")),
        limit: None,
        settings,
        plugins: root.join("config/pinhole/plugins.json"),
        adopt: None,
        downloads: 2,
        sync_every: None,
        given: None,
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
    assert_eq!(shown["id"], format!("folder:{}", photos.display()));
    let read = window.event(5, |e| e["event"] == "library_moved");
    assert_eq!(read["library"], json!(library));

    // The library is made from the folder, thumbnails and all (perhaps
    // before this window was there to hear of it).
    let deadline = Instant::now() + Duration::from_secs(60);
    loop {
        let sources = window.ask("sources", json!({}));
        let made = &sources["result"]["sources"][0];
        let count = 2 + has_heic as u64;
        if made["count"] == count && made["thumbs"] == count {
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
    assert!(!compatible.join("IMG_0001.jpg").exists());
    assert_eq!(std::fs::read(compatible.join("IMG_0002.jpg")).unwrap(), b"new");

    // A JPEG of a photo that is one is its full: here the file itself.
    let jpeg = window.ask("fetch", json!({"asset": "a.jpg", "kind": "jpeg"}));
    assert_eq!(jpeg["ok"], true, "{jpeg:#}");
    assert_eq!(jpeg["result"]["path"], json!(photos.join("a.jpg")));
    assert_eq!(jpeg["result"]["type"], "public.jpeg");
    assert_eq!(jpeg["result"]["rendition"], "a.jpg");
    assert_eq!(window.ask("fetch", json!({"asset": "nope.jpg", "kind": "jpeg"}))["error"]["code"], "not_found");
    if has_heic {
        // A HEIC's is made into the cache beside full and counted with it,
        // keeping the date it was taken. Asked for by three at once, it is
        // made once, all three waiting for it.
        let ids: Vec<u64> = (0..3).map(|i| window.next + i).collect();
        for id in &ids {
            writeln!(window.out, "{}", json!({"id": id, "method": "fetch", "params": {"asset": "c.heic", "kind": "jpeg"}})).unwrap();
        }
        window.next += ids.len() as u64;
        let deadline = Instant::now() + Duration::from_secs(30);
        let mut answers = Vec::new();
        while answers.len() < ids.len() {
            assert!(Instant::now() < deadline, "no JPEG: {:#?}", window.heard);
            if let Some(line) = window.line() {
                if line["id"].as_u64().is_some_and(|id| ids.contains(&id)) {
                    answers.push(line);
                } else {
                    window.heard.push(line);
                }
            }
        }
        for made in &answers {
            assert_eq!(made["ok"], true, "{made:#}");
            assert_eq!(made["result"]["path"], answers[0]["result"]["path"]);
            assert_eq!(made["result"]["type"], "public.jpeg");
            assert_eq!(made["result"]["rendition"], "made");
            // None found it made already: all waited for the one.
            assert_eq!(made["result"]["cached"], false);
        }
        let path = answers[0]["result"]["path"].as_str().unwrap().to_owned();
        assert!(path.starts_with(root.join("data/pinhole/cache/full").to_str().unwrap()), "{path}");
        assert!(path.ends_with("-jpeg.jpg"), "{path}");
        assert_eq!(identify(&path, "%m"), "JPEG");
        assert_eq!(identify(&path, "%[EXIF:DateTimeOriginal]"), "2024:05:06 07:08:09");
        assert_eq!(colour(&path), [40, 40, 200]);
        let ran = std::fs::read_to_string(&magicks).unwrap_or_default();
        assert_eq!(ran.lines().filter(|l| l.contains("-jpeg.part.jpg")).count(), 1, "{ran}");
        let again = window.ask("fetch", json!({"asset": "c.heic", "kind": "jpeg"}));
        assert_eq!(again["result"]["cached"], true, "{again:#}");
        assert_eq!(again["result"]["path"], json!(path));
        let storage = window.ask("storage", json!({}));
        assert_eq!(storage["result"]["kinds"]["full"], std::fs::metadata(&path).unwrap().len(), "{storage:#}");
    }
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
    // Asked of another source's photos of the same id: not done here.
    let elsewhere = window.ask("set_favorite", json!({"assets": ["a.jpg"], "favorite": false, "source": "folder:/elsewhere"}));
    assert_eq!(elsewhere["error"]["code"], "other_source", "{elsewhere:#}");
    assert!(favourite(&library, "a.jpg"));
    // A photo asked for as another source's (or of a library that says
    // whose it is not): not this one's.
    let own = format!("folder:{}", photos.display());
    let asked = window.ask("fetch", json!({"asset": "a.jpg", "kind": "original", "source": own}));
    assert_eq!(asked["ok"], true, "{asked:#}");
    for other in ["folder:/elsewhere", ""] {
        let asked = window.ask("fetch", json!({"asset": "a.jpg", "kind": "original", "source": other}));
        assert_eq!(asked["error"]["code"], "other_source", "{asked:#}");
    }

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

    // Everything to another folder: the library first, every window told,
    // then the photos; the settings say the new one, and no move is left.
    let moved_to = root.join("moved");
    let set = window.ask("set_storage", json!({"location": moved_to}));
    assert_eq!(set["ok"], true, "{set:#}");
    assert_eq!(set["result"]["location"], json!(moved_to));
    let into = moved_to.to_str().unwrap().to_owned();
    let moved = window.event(30, |e| e["event"] == "library_moved" && e["library"].as_str().is_some_and(|l| l.starts_with(&into)));
    // And where they all went, for a window keeping to another's.
    assert_eq!(moved["libraries"]["to"], json!(moved_to.join("library")), "{moved:#}");
    let deadline = Instant::now() + Duration::from_secs(30);
    while !window.ask("storage", json!({}))["result"]["moving"].is_null() {
        assert!(Instant::now() < deadline, "still moving");
        std::thread::sleep(Duration::from_millis(100));
    }
    let written: Value = serde_json::from_str(&std::fs::read_to_string(root.join("config/pinhole/settings.json")).unwrap()).unwrap();
    assert_eq!(written["location"], json!(moved_to));
    assert!(written.get("moving").is_none(), "{written:#}");
    assert!(!root.join("data/pinhole/library").exists());
    let after = window.ask("fetch", json!({"asset": "a.jpg", "kind": "medium"}));
    assert_eq!(after["ok"], true, "{after:#}");
    assert!(after["result"]["path"].as_str().unwrap().starts_with(moved_to.to_str().unwrap()), "{after:#}");

    // A move asked for, and another source shown straight after, before it
    // has begun: whichever goes first, the source shown and the library
    // read are the same one's, in the new folder.
    let again = root.join("again");
    writeln!(window.out, "{}", json!({"id": window.next, "method": "set_storage", "params": {"location": again}})).unwrap();
    window.next += 1;
    let shown = window.ask("set_source", json!({"kind": "folder", "path": photos}));
    assert_eq!(shown["ok"], true, "{shown:#}");
    let deadline = Instant::now() + Duration::from_secs(30);
    let status = loop {
        let status = window.ask("status", json!({}));
        let storage = window.ask("storage", json!({}));
        if storage["result"]["moving"].is_null() && storage["result"]["location"] == json!(again) {
            break status;
        }
        assert!(Instant::now() < deadline, "{storage:#}");
        std::thread::sleep(Duration::from_millis(100));
    };
    let library = PathBuf::from(status["result"]["library"].as_str().unwrap());
    assert!(library.starts_with(&again), "{status:#}");
    assert_eq!(status["result"]["source"]["path"], json!(photos), "{status:#}");
    let owner: String = rusqlite::Connection::open(library.join("library.sqlite"))
        .unwrap()
        .query_row("SELECT value FROM meta WHERE key = 'source'", [], |r| r.get(0))
        .unwrap();
    assert_eq!(owner, format!("folder:{}", photos.display()));
    let red = window.ask("fetch", json!({"asset": "a.jpg", "kind": "original"}));
    assert_eq!(red["result"]["path"], json!(photos.join("a.jpg")), "{red:#}");

    // A window that asks and asks and never reads: no other waits on it,
    // and it is hung up on.
    let stalled = Window::connect(&socket);
    let mut asking = stalled.out.try_clone().unwrap();
    std::thread::spawn(move || {
        let pings: String = (0..20_000).map(|id| format!("{}\n", json!({"id": id, "method": "ping"}))).collect();
        let _ = asking.write_all(pings.as_bytes());
    });
    std::thread::sleep(Duration::from_millis(500));
    let asked = Instant::now();
    let set = window.ask("set_favorite", json!({"assets": ["a.jpg"], "favorite": false}));
    assert_eq!(set["result"]["changed"], 1, "{set:#}");
    window.event(5, |e| e["event"] == "library_changed");
    assert!(asked.elapsed() < Duration::from_secs(2), "waited {:?} on a window that does not read", asked.elapsed());
    let mut lines = stalled.lines;
    lines.get_ref().set_read_timeout(Some(Duration::from_secs(10))).unwrap();
    let mut rest = Vec::new();
    assert!(std::io::Read::read_to_end(&mut lines, &mut rest).is_ok(), "not hung up on");
    // Gone from those told the news.
    let deadline = Instant::now() + Duration::from_secs(10);
    while window.ask("status", json!({}))["result"]["windows"] != 1 {
        assert!(Instant::now() < deadline, "the window hung up on is still told the news");
        std::thread::sleep(Duration::from_millis(100));
    }

    // A service given a library (--library, as a window given one starts
    // it) of another folder than the one it shows: none of that library's
    // photos is fetched from this folder, though one has the same name.
    let given = root.join("given");
    let theirs = root.join("theirs");
    std::fs::create_dir_all(&theirs).unwrap();
    photo(&theirs.join("a.jpg"), [200, 200, 40]);
    std::fs::create_dir_all(&given).unwrap();
    let mut conn = pinhole_core::db::open(&given.join("library.sqlite")).unwrap();
    let their_source = FolderSource::new(theirs.clone());
    pinhole_core::sync::claim(&their_source, &mut conn, &given).unwrap();
    pinhole_core::sync::list(&their_source, &mut conn, &|_, _| {}, &|| false).unwrap();
    drop(conn);
    assert_eq!(pinhole_core::storage::library_owner(&given), Some(format!("folder:{}", theirs.display())));
    let first_socket = socket.clone();
    let given_socket = PathBuf::from("/tmp").join(format!("pinhole-service-given-{}.sock", std::process::id()));
    let socket = given_socket.clone();
    let config = Config {
        library: given.clone(),
        cache: root.join("given-cache"),
        location: root.join("given-location"),
        socket: socket.clone(),
        source: Arc::new(FolderSource::new(photos.clone())),
        kinds: pinholed::builtin_kinds(PathBuf::from("/nonexistent/pinhole-auth")),
        limit: None,
        settings: root.join("config/pinhole/given.json"),
        plugins: root.join("config/pinhole/plugins.json"),
        adopt: None,
        downloads: 2,
        sync_every: None,
        given: None,
    };
    std::thread::spawn(move || service::run(config));
    let mut pinned = Window::connect(&socket);
    let theirs_id = format!("folder:{}", theirs.display());
    for asked in [json!({"asset": "a.jpg", "kind": "original", "source": theirs_id}), json!({"asset": "a.jpg", "kind": "original"})] {
        let fetched = pinned.ask("fetch", asked);
        assert_eq!(fetched["error"]["code"], "other_source", "{fetched:#}");
    }

    let _ = std::fs::remove_file(&first_socket);
    let _ = std::fs::remove_file(&given_socket);
    let _ = std::fs::remove_dir_all(&root);
}
