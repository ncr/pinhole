//! What is kept of a library that follows it to every computer it is shown
//! on: the names People gave faces, Series' series made by hand
//! (docs/plugins.md, Synced; docs/service.md, Synced).
//!
//! Each library has a folder of its own,
//! `~/.local/state/pinhole/synced/<kind>-<name>/`, with a file to each plugin:
//! its `values`, and the `clock` of
//! each key, when and on which computer it was last set. A key set to
//! nothing keeps its clock, so taking one out travels as setting it does.
//! Two computers that set one key between two syncs agree on the later,
//! whatever order they hear of each other in: a key is merged whole.
//!
//! The window writes them as plugins set keys, the service as it brings
//! what other computers did (`carry`), each by reading the file, merging
//! into it and writing it again in one step, under the folder's lock: no
//! writer loses another's change.
//!
//! Between computers it goes as the library's source can carry it
//! (`Carrier`): iCloud in a Pinhole folder in that account's iCloud Drive,
//! one file to each computer, which writes its own and reads the others'.
//! A library whose source carries nothing keeps it on this computer.

use std::collections::BTreeMap;
use std::fs::File;
use std::io::Write;
use std::path::{Path, PathBuf};
use std::time::{SystemTime, UNIX_EPOCH};

use anyhow::{anyhow, Context, Result};
use serde_json::{json, Map, Value};

/// What a computer's file says it is.
pub const FORMAT: u64 = 1;

/// When and where a key was last set: milliseconds since 1970, and the
/// computer (its `device` id). The later wins; at the same millisecond,
/// the greater id, so every computer picks the same one.
pub type Stamp = (u64, String);

/// One plugin's of one library.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Doc {
    pub values: Map<String, Value>,
    pub clock: BTreeMap<String, Stamp>,
}

impl Doc {
    pub fn from_json(value: &Value) -> Result<Doc> {
        let object = value.as_object().ok_or_else(|| anyhow!("not an object"))?;
        let values = match object.get("values") {
            Some(Value::Object(values)) => values.clone(),
            None => Map::new(),
            Some(_) => return Err(anyhow!("values is not an object")),
        };
        let mut clock = BTreeMap::new();
        if let Some(stamps) = object.get("clock") {
            let stamps = stamps.as_object().ok_or_else(|| anyhow!("clock is not an object"))?;
            for (key, stamp) in stamps {
                let (Some(at), Some(by)) = (stamp.get(0).and_then(Value::as_u64), stamp.get(1).and_then(Value::as_str)) else {
                    return Err(anyhow!("the clock of {key} is not [milliseconds, computer]"));
                };
                clock.insert(key.clone(), (at, by.to_owned()));
            }
        }
        Ok(Doc { values, clock })
    }

    pub fn to_json(&self) -> Value {
        let clock: Map<String, Value> = self.clock.iter().map(|(k, (at, by))| (k.clone(), json!([at, by]))).collect();
        json!({"values": self.values, "clock": clock})
    }

    /// `other`'s keys that are later than these, taken: the keys that
    /// changed.
    pub fn merge(&mut self, other: &Doc) -> Vec<String> {
        self.merge_stamped(other).0
    }

    /// As `merge`, and whether any key's clock moved on (a key set again
    /// to what it was changes no value, but its clock must travel).
    pub fn merge_stamped(&mut self, other: &Doc) -> (Vec<String>, bool) {
        let mut changed = Vec::new();
        let mut stamped = false;
        for (key, stamp) in &other.clock {
            if self.clock.get(key).is_some_and(|mine| mine >= stamp) {
                continue;
            }
            stamped = true;
            let before = self.values.get(key).cloned();
            match other.values.get(key) {
                Some(value) if !value.is_null() => self.values.insert(key.clone(), value.clone()),
                _ => self.values.remove(key),
            };
            self.clock.insert(key.clone(), stamp.clone());
            if self.values.get(key) != before.as_ref() {
                changed.push(key.clone());
            }
        }
        (changed, stamped)
    }

    /// `changes` set here, by computer `by`: each a millisecond later than
    /// it was last set, wherever that was, so it wins over what this
    /// computer knew of it. Null takes a key out.
    pub fn set(&mut self, changes: &Map<String, Value>, by: &str) {
        let now = now_millis();
        for (key, value) in changes {
            let at = self.clock.get(key).map_or(now, |(at, _)| now.max(at + 1));
            if value.is_null() {
                self.values.remove(key);
            } else {
                self.values.insert(key.clone(), value.clone());
            }
            self.clock.insert(key.clone(), (at, by.to_owned()));
        }
    }
}

pub fn now_millis() -> u64 {
    SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_millis() as u64).unwrap_or(0)
}

fn state_home() -> PathBuf {
    std::env::var_os("XDG_STATE_HOME")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from(std::env::var_os("HOME").unwrap_or_default()).join(".local/state"))
}

/// Where every library's is kept.
pub fn root() -> PathBuf {
    state_home().join("pinhole/synced")
}

/// A library's folder of them, named as a plugin's cacheDir names it:
/// library/icloud/<apple id> is icloud-<apple id>.
pub fn library_dir(library: &Path) -> PathBuf {
    root().join(crate::workers::cache_name(library))
}

/// A plugin's file there.
pub fn file(dir: &Path, plugin: &str) -> PathBuf {
    dir.join(format!("{plugin}.json"))
}

/// A plugin's id or the window's, as a file may be named after it.
pub fn is_name(name: &str) -> bool {
    !name.is_empty()
        && name.len() <= 128
        && !name.starts_with('.')
        && name.chars().all(|c| c.is_ascii_alphanumeric() || matches!(c, '.' | '_' | '-'))
}

/// This computer's id, made the first time it is asked for: twelve hex
/// digits, in ~/.local/state/pinhole/device. The window reads the same.
pub fn device() -> Result<String> {
    let path = state_home().join("pinhole/device");
    if let Ok(text) = std::fs::read_to_string(&path) {
        let id = text.trim();
        if !id.is_empty() {
            return Ok(id.to_owned());
        }
    }
    std::fs::create_dir_all(path.parent().unwrap())?;
    let id = format!("{:012x}", rand::random::<u64>() & 0xffff_ffff_ffff);
    match std::fs::OpenOptions::new().write(true).create_new(true).open(&path) {
        Ok(mut f) => {
            f.write_all(format!("{id}\n").as_bytes())?;
            Ok(id)
        }
        // Made meanwhile, by the window: its, once it is written.
        Err(e) if e.kind() == std::io::ErrorKind::AlreadyExists => {
            for _ in 0..50 {
                let id = std::fs::read_to_string(&path)?.trim().to_owned();
                if !id.is_empty() {
                    return Ok(id);
                }
                std::thread::sleep(std::time::Duration::from_millis(20));
            }
            Err(anyhow!("{} is empty", path.display()))
        }
        Err(e) => Err(e).with_context(|| format!("making {}", path.display())),
    }
}

/// What this computer is called, for the name of its file elsewhere.
pub fn computer() -> String {
    let mut buffer = [0u8; 256];
    // SAFETY: the buffer is as long as it is said to be.
    let ok = unsafe { libc::gethostname(buffer.as_mut_ptr().cast(), buffer.len()) } == 0;
    let name = if ok {
        let end = buffer.iter().position(|&b| b == 0).unwrap_or(buffer.len());
        String::from_utf8_lossy(&buffer[..end]).into_owned()
    } else {
        String::new()
    };
    let name: String = name
        .split('.')
        .next()
        .unwrap_or_default()
        .chars()
        .filter(|c| c.is_ascii_alphanumeric() || *c == '-')
        .take(40)
        .collect();
    if name.is_empty() { "computer".into() } else { name }
}

/// The folder's lock, held while one of its files is read, merged into
/// and written again (by the window too: the same file).
pub struct Lock(File);

impl Lock {
    pub fn take(dir: &Path) -> Result<Lock> {
        std::fs::create_dir_all(dir).with_context(|| format!("making {}", dir.display()))?;
        let file = File::options().create(true).append(true).open(dir.join(".lock"))?;
        // SAFETY: a descriptor of a file open for as long as the lock is.
        if unsafe { libc::flock(std::os::fd::AsRawFd::as_raw_fd(&file), libc::LOCK_EX) } != 0 {
            return Err(std::io::Error::last_os_error()).context("locking the synced folder");
        }
        Ok(Lock(file))
    }
}

impl Drop for Lock {
    fn drop(&mut self) {
        // SAFETY: as above.
        unsafe { libc::flock(std::os::fd::AsRawFd::as_raw_fd(&self.0), libc::LOCK_UN) };
    }
}

/// A file as it is (empty if there is none). One that cannot be read is
/// an error, never taken as empty: writing over it would lose it.
pub fn load(path: &Path) -> Result<Doc> {
    match std::fs::read(path) {
        Ok(bytes) => {
            let value: Value = serde_json::from_slice(&bytes).with_context(|| format!("reading {}", path.display()))?;
            Doc::from_json(&value).with_context(|| format!("reading {}", path.display()))
        }
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(Doc::default()),
        Err(e) => Err(e).with_context(|| format!("reading {}", path.display())),
    }
}

/// Written beside it, then renamed over it: never half written.
pub fn save(path: &Path, doc: &Doc) -> Result<()> {
    let dir = path.parent().ok_or_else(|| anyhow!("no folder"))?;
    std::fs::create_dir_all(dir)?;
    let part = dir.join(format!(".{}.part", path.file_name().unwrap().to_string_lossy()));
    std::fs::write(&part, serde_json::to_vec(&doc.to_json())?)?;
    std::fs::rename(&part, path).with_context(|| format!("writing {}", path.display()))
}

/// `other` merged into a plugin's file: the keys that changed.
pub fn merge_into(dir: &Path, plugin: &str, other: &Doc) -> Result<Vec<String>> {
    let _lock = Lock::take(dir)?;
    let path = file(dir, plugin);
    let mut doc = load(&path)?;
    let (changed, stamped) = doc.merge_stamped(other);
    if stamped {
        save(&path, &doc)?;
    }
    Ok(changed)
}

/// `changes` set in a plugin's file by this computer (the window does the
/// same itself; tests and `pinhole` commands use this).
pub fn set(dir: &Path, plugin: &str, changes: &Map<String, Value>, by: &str) -> Result<()> {
    let _lock = Lock::take(dir)?;
    let path = file(dir, plugin);
    let mut doc = load(&path)?;
    doc.set(changes, by);
    save(&path, &doc)
}

/// Every plugin's in a library's folder, by plugin, and the plugins whose
/// file cannot be read: left out (and said), never taken as empty.
pub fn all(dir: &Path) -> Result<(BTreeMap<String, Doc>, Vec<String>)> {
    let mut docs = BTreeMap::new();
    let mut unreadable = Vec::new();
    let entries = match std::fs::read_dir(dir) {
        Ok(entries) => entries,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok((docs, unreadable)),
        Err(e) => return Err(e.into()),
    };
    for entry in entries.flatten() {
        let name = entry.file_name().to_string_lossy().into_owned();
        let Some(plugin) = name.strip_suffix(".json") else { continue };
        if is_name(plugin) {
            match load(&entry.path()) {
                Ok(doc) => {
                    docs.insert(plugin.to_owned(), doc);
                }
                Err(e) => {
                    eprintln!("pinholed: synced: {e:#}");
                    unreadable.push(plugin.to_owned());
                }
            }
        }
    }
    Ok((docs, unreadable))
}

/// How the files look now (their names, sizes and times): a change here
/// to carry when it changes.
pub fn signature(dir: &Path) -> Vec<(String, u64, SystemTime)> {
    let mut seen: Vec<_> = std::fs::read_dir(dir)
        .into_iter()
        .flatten()
        .flatten()
        .filter_map(|e| {
            let name = e.file_name().to_string_lossy().into_owned();
            let meta = e.metadata().ok()?;
            let modified = meta.modified().ok()?;
            (name.ends_with(".json") && !name.starts_with('.')).then_some((name, meta.len(), modified))
        })
        .collect();
    seen.sort();
    seen
}

/// A file elsewhere, and its version there (it changes as it does).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Carried {
    pub name: String,
    pub version: String,
}

/// Where a source keeps what is synced of its library, for its other
/// computers (iCloud: a Pinhole folder in the account's iCloud Drive).
/// Each computer writes one file there, its own, and reads the others'.
pub trait Carrier: Send + Sync {
    /// The files there now, none if there is no folder yet.
    fn list(&self) -> Result<Vec<Carried>>;
    /// One of them, as `list` last named it; `TooBig` rather than more
    /// than `SYNCED_MAX` bytes of it.
    fn read(&self, name: &str) -> Result<Vec<u8>>;
    /// This computer's, made or written over.
    fn write(&self, name: &str, bytes: &[u8]) -> Result<()>;
}

/// What the last carrying left, in the library's folder (.carried.json):
/// the plugins' as last sent (a hash), and each other computer's file as
/// last read (its version).
#[derive(Default)]
struct Mark {
    sent: Option<u64>,
    seen: BTreeMap<String, String>,
}

impl Mark {
    fn path(dir: &Path) -> PathBuf {
        dir.join(".carried.json")
    }

    fn load(dir: &Path) -> Mark {
        let Ok(bytes) = std::fs::read(Mark::path(dir)) else { return Mark::default() };
        let Ok(value) = serde_json::from_slice::<Value>(&bytes) else { return Mark::default() };
        Mark {
            sent: value["sent"].as_str().and_then(|h| u64::from_str_radix(h, 16).ok()),
            seen: value["seen"]
                .as_object()
                .map(|o| o.iter().filter_map(|(k, v)| Some((k.clone(), v.as_str()?.to_owned()))).collect())
                .unwrap_or_default(),
        }
    }

    fn save(&self, dir: &Path) -> Result<()> {
        let value = json!({"sent": self.sent.map(|h| format!("{h:016x}")), "seen": self.seen});
        let part = dir.join(".carried.json.part");
        std::fs::write(&part, value.to_string())?;
        std::fs::rename(&part, Mark::path(dir))?;
        Ok(())
    }
}

/// FNV-1a: the same bytes give the same hash on every computer and build.
fn hash(bytes: &[u8]) -> u64 {
    bytes.iter().fold(0xcbf2_9ce4_8422_2325u64, |h, b| (h ^ u64::from(*b)).wrapping_mul(0x0000_0100_0000_01b3))
}

/// This computer's file's name: what it is called, then its id.
pub fn own_name(device: &str, computer: &str) -> String {
    format!("{computer}-{device}.json")
}

fn is_own(name: &str, device: &str) -> bool {
    name == format!("{device}.json") || name.ends_with(&format!("-{device}.json"))
}

/// Whether a file there is a computer's (`own_name`, or its id alone as
/// before): its id last, twelve hex digits. Nothing else in the folder is
/// read.
fn is_computers(name: &str) -> bool {
    let Some(stem) = name.strip_suffix(".json") else { return false };
    let id = stem.rsplit_once('-').map_or(stem, |(_, id)| id);
    id.len() == 12 && id.bytes().all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
}

/// The most a computer's file is read of (`Carrier::read`): years of
/// faces named are a few megabytes.
pub const SYNCED_MAX: u64 = 64 << 20;

/// A carrier's `read` of a file bigger than `SYNCED_MAX`, stopped there:
/// that version of it is never got again.
#[derive(Debug)]
pub struct TooBig(pub String);

impl std::fmt::Display for TooBig {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{}, more than {} MB", self.0, SYNCED_MAX >> 20)
    }
}

impl std::error::Error for TooBig {}

/// How a round of carrying went.
#[derive(Debug, Default, PartialEq)]
pub struct Carrying {
    /// The plugins whose files changed, with what came from elsewhere.
    pub changed: Vec<String>,
    /// The other computers' files read.
    pub read: Vec<String>,
    /// This computer's file was written.
    pub sent: bool,
}

/// One round: the other computers' files that changed since they were
/// last read merged into this one's, then this computer's written, if
/// anything in it changed since it last was.
pub fn carry(dir: &Path, carrier: &dyn Carrier, device: &str, computer: &str) -> Result<Carrying> {
    std::fs::create_dir_all(dir)?;
    let mut mark = Mark::load(dir);
    let mut done = Carrying::default();
    let there = carrier.list()?;
    let own = own_name(device, computer);
    for file in &there {
        if is_own(&file.name, device) || !is_computers(&file.name) || mark.seen.get(&file.name) == Some(&file.version) {
            continue;
        }
        // One that cannot be got (the network, a file Drive will not
        // give) is tried again next round; the rest go on meanwhile.
        let bytes = match carrier.read(&file.name) {
            Ok(bytes) => bytes,
            // Too big to be one: not got again until it changes.
            Err(e) if e.downcast_ref::<TooBig>().is_some() => {
                eprintln!("pinholed: synced: {} could not be read: {e:#}", file.name);
                mark.seen.insert(file.name.clone(), file.version.clone());
                continue;
            }
            Err(e) => {
                eprintln!("pinholed: synced: {} could not be got: {e:#}", file.name);
                continue;
            }
        };
        // One that is not what it should be is read again only once it
        // changes.
        let checked = if bytes.len() as u64 > SYNCED_MAX {
            Err(anyhow!("{} MB, more than {} MB", bytes.len() >> 20, SYNCED_MAX >> 20))
        } else {
            serde_json::from_slice::<Value>(&bytes).map_err(anyhow::Error::from).and_then(|v| plugins_of(&v))
        };
        match checked {
            Ok(plugins) => {
                let mut all_in = true;
                for (plugin, doc) in plugins {
                    match merge_into(dir, &plugin, &doc) {
                        Ok(changed) => {
                            if !changed.is_empty() && !done.changed.contains(&plugin) {
                                done.changed.push(plugin);
                            }
                        }
                        // This computer's file of it cannot be read: never
                        // written over; the rest are merged, and this one
                        // tried again next round.
                        Err(e) => {
                            eprintln!("pinholed: synced: {plugin} of {}: {e:#}", file.name);
                            all_in = false;
                        }
                    }
                }
                done.read.push(file.name.clone());
                if !all_in {
                    continue;
                }
            }
            Err(e) => eprintln!("pinholed: synced: {} could not be read: {e:#}", file.name),
        }
        mark.seen.insert(file.name.clone(), file.version.clone());
    }
    mark.seen.retain(|name, _| there.iter().any(|f| &f.name == name));

    let (docs, unreadable) = all(dir)?;
    let mut plugins: Map<String, Value> = docs.iter().map(|(p, d)| (p.clone(), d.to_json())).collect();
    let present = there.iter().any(|f| f.name == own);
    // A plugin's file here that cannot be read goes as this computer last
    // sent it, never left out of what is there: that may be the only copy
    // of it left. Should that not be got, nothing is sent this round.
    if !unreadable.is_empty() && present {
        let last = carrier
            .read(&own)
            .and_then(|bytes| Ok(serde_json::from_slice::<Value>(&bytes)?))
            .and_then(|v| v["plugins"].as_object().cloned().ok_or_else(|| anyhow!("no plugins")));
        match last {
            Ok(last) => {
                for plugin in &unreadable {
                    if let Some(doc) = last.get(plugin) {
                        plugins.insert(plugin.clone(), doc.clone());
                    }
                }
            }
            Err(e) => {
                eprintln!("pinholed: synced: {own} not sent: a file here cannot be read, nor what was sent got: {e:#}");
                mark.save(dir)?;
                return Ok(done);
            }
        }
    }
    let plugins = Value::Object(plugins);
    let sum = hash(plugins.to_string().as_bytes());
    // Nothing kept and nothing sent before: no folder made for nothing.
    if (mark.sent != Some(sum) || !present) && !(docs.is_empty() && !present) {
        let body = json!({
            "pinhole": FORMAT,
            "device": device,
            "computer": computer,
            "written": now_millis(),
            "plugins": plugins,
        });
        carrier.write(&own, &serde_json::to_vec(&body)?)?;
        mark.sent = Some(sum);
        done.sent = true;
    }
    mark.save(dir)?;
    Ok(done)
}

/// A computer's file's plugins, each checked.
fn plugins_of(value: &Value) -> Result<Vec<(String, Doc)>> {
    if value["pinhole"].as_u64().is_some_and(|f| f > FORMAT) {
        return Err(anyhow!("written by a newer Pinhole"));
    }
    let plugins = value["plugins"].as_object().ok_or_else(|| anyhow!("no plugins"))?;
    plugins
        .iter()
        .filter(|(plugin, _)| is_name(plugin))
        .map(|(plugin, doc)| Ok((plugin.clone(), Doc::from_json(doc)?)))
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::HashMap;
    use std::sync::Mutex;

    fn doc(entries: &[(&str, Option<Value>, u64, &str)]) -> Doc {
        let mut d = Doc::default();
        for (k, v, at, by) in entries {
            if let Some(v) = v {
                d.values.insert(k.to_string(), v.clone());
            }
            d.clock.insert(k.to_string(), (*at, by.to_string()));
        }
        d
    }

    #[test]
    fn the_later_of_a_key_wins_whichever_is_heard_first() {
        let a = doc(&[("face", Some(json!({"person": "Anna"})), 10, "a"), ("other", Some(json!(1)), 5, "a")]);
        let b = doc(&[("face", Some(json!({"person": "Ola"})), 20, "b"), ("other", None, 4, "b")]);
        let mut ab = a.clone();
        ab.merge(&b);
        let mut ba = b.clone();
        ba.merge(&a);
        assert_eq!(ab, ba);
        assert_eq!(ab.values["face"], json!({"person": "Ola"}));
        assert_eq!(ab.values["other"], json!(1));
    }

    #[test]
    fn taking_a_key_out_travels_and_a_tie_goes_to_the_greater_computer() {
        let mut here = doc(&[("face", Some(json!("Anna")), 10, "a")]);
        let gone = doc(&[("face", None, 11, "b")]);
        assert_eq!(here.merge(&gone), ["face"]);
        assert!(!here.values.contains_key("face"));
        assert_eq!(here.merge(&doc(&[("face", Some(json!("Ola")), 11, "a")])), Vec::<String>::new());
        assert_eq!(here.merge(&doc(&[("face", Some(json!("Ola")), 11, "c")])), ["face"]);
    }

    #[test]
    fn a_key_set_here_is_later_than_any_seen_of_it() {
        let mut here = doc(&[("face", Some(json!("Anna")), u64::MAX / 2, "b")]);
        let mut changes = Map::new();
        changes.insert("face".into(), json!("Ola"));
        changes.insert("new".into(), Value::Null);
        here.set(&changes, "a");
        assert_eq!(here.clock["face"], (u64::MAX / 2 + 1, "a".to_string()));
        assert_eq!(here.values["face"], json!("Ola"));
        assert!(!here.values.contains_key("new") && here.clock.contains_key("new"));
    }

    /// A folder elsewhere, in memory.
    #[derive(Default)]
    struct Elsewhere {
        files: Mutex<HashMap<String, (Vec<u8>, u64)>>,
        writes: Mutex<u64>,
    }

    impl Carrier for Elsewhere {
        fn list(&self) -> Result<Vec<Carried>> {
            Ok(self.files.lock().unwrap().iter().map(|(n, (_, v))| Carried { name: n.clone(), version: v.to_string() }).collect())
        }
        fn read(&self, name: &str) -> Result<Vec<u8>> {
            Ok(self.files.lock().unwrap()[name].0.clone())
        }
        fn write(&self, name: &str, bytes: &[u8]) -> Result<()> {
            *self.writes.lock().unwrap() += 1;
            let mut files = self.files.lock().unwrap();
            let version = files.get(name).map_or(1, |(_, v)| v + 1);
            files.insert(name.to_owned(), (bytes.to_vec(), version));
            Ok(())
        }
    }

    fn folder(name: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("pinhole-synced-{name}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        dir
    }

    fn named(face: &str, name: &str) -> Map<String, Value> {
        let mut m = Map::new();
        m.insert(face.into(), json!({"person": name}));
        m
    }

    #[test]
    fn names_given_on_one_computer_reach_another_and_back() {
        let (work, home) = (folder("work"), folder("home"));
        let drive = Elsewhere::default();
        set(&work, "pinhole.people", &named("f1", "Anna"), "w").unwrap();
        let first = carry(&work, &drive, "0000000000aa", "spawner").unwrap();
        assert!(first.sent && first.changed.is_empty());
        assert!(drive.files.lock().unwrap().contains_key("spawner-0000000000aa.json"));

        let at_home = carry(&home, &drive, "0000000000bb", "fixer").unwrap();
        assert_eq!(at_home.changed, ["pinhole.people"]);
        assert_eq!(load(&file(&home, "pinhole.people")).unwrap().values["f1"], json!({"person": "Anna"}));
        // What came is sent back as this computer's: the same, both ways.
        assert!(at_home.sent);

        set(&home, "pinhole.people", &named("f1", "Ola"), "h").unwrap();
        carry(&home, &drive, "0000000000bb", "fixer").unwrap();
        let back = carry(&work, &drive, "0000000000aa", "spawner").unwrap();
        assert_eq!(back.changed, ["pinhole.people"]);
        assert_eq!(load(&file(&work, "pinhole.people")).unwrap().values["f1"], json!({"person": "Ola"}));

        // Nothing new: nothing changes, nothing more is written.
        let writes = *drive.writes.lock().unwrap();
        assert_eq!(carry(&work, &drive, "0000000000aa", "spawner").unwrap(), Carrying::default());
        let quiet = carry(&home, &drive, "0000000000bb", "fixer").unwrap();
        assert!(quiet.changed.is_empty() && !quiet.sent, "{quiet:?}");
        assert_eq!(carry(&work, &drive, "0000000000aa", "spawner").unwrap(), Carrying::default());
        assert_eq!(carry(&home, &drive, "0000000000bb", "fixer").unwrap(), Carrying::default());
        assert_eq!(*drive.writes.lock().unwrap(), writes);
        let _ = std::fs::remove_dir_all(&work);
        let _ = std::fs::remove_dir_all(&home);
    }

    /// People's face taken out of a group and Series' series each under a
    /// key of their own: what two computers did before they heard of each
    /// other is all kept on both.
    #[test]
    fn two_computers_rejections_and_series_made_apart_are_both_kept() {
        let (work, home) = (folder("apart-work"), folder("apart-home"));
        let drive = Elsewhere::default();
        let change = |key: &str, value: Value| Map::from_iter([(key.to_string(), value)]);
        set(&work, "pinhole.people", &change("g", json!({"person": "Anna"})), "w").unwrap();
        carry(&work, &drive, "0000000000aa", "spawner").unwrap();
        carry(&home, &drive, "0000000000bb", "fixer").unwrap();
        // Offline, each takes another face out of the same group and makes
        // another series.
        set(&work, "pinhole.people", &change("reject\tg\tf1", json!(true)), "w").unwrap();
        set(&home, "pinhole.people", &change("reject\tg\tf2", json!(true)), "h").unwrap();
        set(&work, "pinhole.series", &change("group\tw1", json!(["a", "b"])), "w").unwrap();
        set(&home, "pinhole.series", &change("group\th1", json!(["c", "d"])), "h").unwrap();
        set(&home, "pinhole.series", &change("apart\te", json!(true)), "h").unwrap();
        for _ in 0..2 {
            carry(&work, &drive, "0000000000aa", "spawner").unwrap();
            carry(&home, &drive, "0000000000bb", "fixer").unwrap();
        }
        for dir in [&work, &home] {
            let people = load(&file(dir, "pinhole.people")).unwrap().values;
            assert_eq!(people.keys().collect::<Vec<_>>(), ["g", "reject\tg\tf1", "reject\tg\tf2"]);
            let series = load(&file(dir, "pinhole.series")).unwrap().values;
            assert_eq!(series.keys().collect::<Vec<_>>(), ["apart\te", "group\th1", "group\tw1"]);
        }
        let _ = std::fs::remove_dir_all(&work);
        let _ = std::fs::remove_dir_all(&home);
    }

    #[test]
    fn a_computer_with_nothing_kept_makes_nothing_elsewhere() {
        let dir = folder("empty");
        let drive = Elsewhere::default();
        assert_eq!(carry(&dir, &drive, "0000000000cc", "fixer").unwrap(), Carrying::default());
        assert!(drive.files.lock().unwrap().is_empty());
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn a_file_elsewhere_that_cannot_be_read_spoils_nothing() {
        let dir = folder("bad");
        let drive = Elsewhere::default();
        drive.write("other-0000000000cc.json", b"{\"plugins\": {\"../evil\": {}, \"me.ok\": 3}}").unwrap();
        set(&dir, "pinhole.people", &named("f1", "Anna"), "h").unwrap();
        let done = carry(&dir, &drive, "0000000000bb", "fixer").unwrap();
        assert!(done.changed.is_empty() && done.sent);
        assert_eq!(load(&file(&dir, "pinhole.people")).unwrap().values["f1"], json!({"person": "Anna"}));
        // Not read again until it changes.
        assert!(carry(&dir, &drive, "0000000000bb", "fixer").unwrap().read.is_empty());
        // Nor anything else someone keeps in the folder, a computer's file
        // by its name or not.
        drive.write("photos-export.json", b"{}").unwrap();
        drive.write("0123456789ab.json", b"{\"plugins\": {}}").unwrap();
        assert_eq!(carry(&dir, &drive, "0000000000bb", "fixer").unwrap().read, ["0123456789ab.json"]);
        let _ = std::fs::remove_dir_all(&dir);
    }

    /// A folder elsewhere with one file it will not give.
    struct Stubborn(Elsewhere);

    impl Carrier for Stubborn {
        fn list(&self) -> Result<Vec<Carried>> {
            self.0.list()
        }
        fn read(&self, name: &str) -> Result<Vec<u8>> {
            if name == "stuck-0000000000cc.json" { Err(anyhow!("no data_token")) } else { self.0.read(name) }
        }
        fn write(&self, name: &str, bytes: &[u8]) -> Result<()> {
            self.0.write(name, bytes)
        }
    }

    #[test]
    fn a_file_elsewhere_that_cannot_be_got_holds_up_nothing_else() {
        let (dir, other) = (folder("stuck"), folder("stuck-other"));
        let drive = Stubborn(Elsewhere::default());
        drive.write("stuck-0000000000cc.json", b"{}").unwrap();
        set(&other, "pinhole.people", &named("f2", "Ola"), "o").unwrap();
        carry(&other, &drive, "0000000000dd", "other").unwrap();
        set(&dir, "pinhole.people", &named("f1", "Anna"), "h").unwrap();
        let done = carry(&dir, &drive, "0000000000bb", "fixer").unwrap();
        assert!(done.sent);
        assert_eq!(done.changed, ["pinhole.people"]);
        assert_eq!(load(&file(&dir, "pinhole.people")).unwrap().values["f2"], json!({"person": "Ola"}));
        // A plugin's file here that cannot be read: the others still go,
        // and it as it was last sent, never left out.
        set(&dir, "me.broken", &named("f9", "Ewa"), "h").unwrap();
        assert!(carry(&dir, &drive, "0000000000bb", "fixer").unwrap().sent);
        std::fs::write(file(&dir, "me.broken"), b"{not json").unwrap();
        set(&dir, "pinhole.people", &named("f3", "Jan"), "h").unwrap();
        assert!(carry(&dir, &drive, "0000000000bb", "fixer").unwrap().sent);
        assert_eq!(std::fs::read(file(&dir, "me.broken")).unwrap(), b"{not json");
        let sent: Value = serde_json::from_slice(&drive.read("fixer-0000000000bb.json").unwrap()).unwrap();
        assert_eq!(sent["plugins"]["me.broken"]["values"]["f9"], json!({"person": "Ewa"}), "{sent}");
        assert_eq!(sent["plugins"]["pinhole.people"]["values"]["f3"], json!({"person": "Jan"}), "{sent}");
        let _ = std::fs::remove_dir_all(&dir);
        let _ = std::fs::remove_dir_all(&other);
    }

    /// A folder elsewhere with one file too big to be a computer's.
    struct Huge(Elsewhere, Mutex<u32>);

    impl Carrier for Huge {
        fn list(&self) -> Result<Vec<Carried>> {
            self.0.list()
        }
        fn read(&self, name: &str) -> Result<Vec<u8>> {
            *self.1.lock().unwrap() += 1;
            Err(TooBig(name.to_owned()).into())
        }
        fn write(&self, name: &str, bytes: &[u8]) -> Result<()> {
            self.0.write(name, bytes)
        }
    }

    #[test]
    fn a_file_too_big_is_got_once_until_it_changes() {
        let dir = folder("huge");
        let drive = Huge(Elsewhere::default(), Mutex::new(0));
        drive.write("huge-0000000000ee.json", b"{}").unwrap();
        carry(&dir, &drive, "0000000000bb", "fixer").unwrap();
        carry(&dir, &drive, "0000000000bb", "fixer").unwrap();
        assert_eq!(*drive.1.lock().unwrap(), 1);
        // Changed: got again.
        drive.write("huge-0000000000ee.json", b"{}").unwrap();
        carry(&dir, &drive, "0000000000bb", "fixer").unwrap();
        assert_eq!(*drive.1.lock().unwrap(), 2);
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn a_file_here_that_cannot_be_read_is_never_written_over() {
        let dir = folder("unreadable");
        std::fs::create_dir_all(&dir).unwrap();
        std::fs::write(file(&dir, "pinhole.people"), b"{not json").unwrap();
        assert!(set(&dir, "pinhole.people", &named("f1", "Anna"), "h").is_err());
        assert_eq!(std::fs::read(file(&dir, "pinhole.people")).unwrap(), b"{not json");
        let _ = std::fs::remove_dir_all(&dir);
    }
}
