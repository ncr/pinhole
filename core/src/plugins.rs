//! The plugins' switches, as the window keeps them in `plugins.json`
//! (`enabled`, by plugin id). pinholed only reads the file: the window
//! writes it, then sends `plugins_changed` (docs/service.md), and each sync
//! round looks at it again in case it was edited by hand. A source kind
//! follows its plugin's switch (kinds.rs).

use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::sync::Mutex;
use std::time::SystemTime;

use serde_json::Value;

/// `~/.config/pinhole/plugins.json`.
pub fn plugins_path() -> PathBuf {
    std::env::var_os("XDG_CONFIG_HOME")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from(std::env::var_os("HOME").unwrap_or_default()).join(".config"))
        .join("pinhole/plugins.json")
}

/// The switches turned one way or the other, by plugin id. A plugin
/// with no entry is not in it: whether it is on is the caller's to say
/// (a built-in is, a plugin of your own is not). No file turns nothing
/// either way. A file there that cannot be read as a JSON object (caught
/// half written, or broken by hand) changes nothing: the switches stay as
/// they were last read from it, so a plugin turned off is not turned on
/// by it. That is logged once, until it reads again.
pub fn switches() -> HashMap<String, bool> {
    switches_in(&plugins_path())
}

pub fn switches_in(path: &Path) -> HashMap<String, bool> {
    /// The switches last read from each file, and whether it was said
    /// that it could not be read since.
    static LAST: Mutex<Option<HashMap<PathBuf, (HashMap<String, bool>, bool)>>> = Mutex::new(None);
    let mut last = LAST.lock().unwrap_or_else(|e| e.into_inner());
    let last = last.get_or_insert_with(HashMap::new);
    let text = match std::fs::read_to_string(path) {
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => {
            last.remove(path);
            return HashMap::new();
        }
        text => text,
    };
    let written = text.map_err(|e| e.to_string()).and_then(|t| {
        serde_json::from_str::<Value>(&t).map_err(|e| e.to_string()).and_then(|v| match v {
            Value::Object(_) => Ok(v),
            _ => Err("not a JSON object".to_owned()),
        })
    });
    let written = match written {
        Ok(written) => written,
        Err(why) => {
            let (kept, said) = last.entry(path.to_owned()).or_default();
            if !*said {
                *said = true;
                eprintln!("pinholed: {} cannot be read ({why}): the plugins' switches stay as they were", path.display());
            }
            return kept.clone();
        }
    };
    let switches: HashMap<String, bool> = written["enabled"]
        .as_object()
        .map(|enabled| enabled.iter().filter_map(|(id, on)| Some((id.clone(), on.as_bool()?))).collect())
        .unwrap_or_default();
    last.insert(path.to_owned(), (switches.clone(), false));
    switches
}

/// When the file last changed, and its size: read again only when these
/// differ from the last time. None when there is no file.
pub fn stamp(path: &Path) -> Option<(SystemTime, u64)> {
    let meta = std::fs::metadata(path).ok()?;
    Some((meta.modified().ok()?, meta.len()))
}

/// Where the plugins are, as the window finds them (`Plugins::builtInDir`
/// and `personalDir`), and where the providers of their sources keep what
/// they keep (docs/source-providers.md).
#[derive(Clone, Debug, PartialEq)]
pub struct Places {
    /// The built-in plugins: `$PINHOLE_BUILTIN_PLUGINS`, else installed
    /// beside pinholed (`../lib/pinhole/plugins`), else this checkout's.
    pub builtin: PathBuf,
    /// Plugins of your own: `~/.config/pinhole/plugins`.
    pub personal: PathBuf,
    /// `~/.local/state/pinhole/sources`: a folder in it for each plugin
    /// with a provider, its `--state-dir`.
    pub state: PathBuf,
}

impl Places {
    /// As the environment and where pinholed is say.
    pub fn found() -> Places {
        let home = || PathBuf::from(std::env::var_os("HOME").unwrap_or_default());
        let xdg = |var: &str, fallback: &str| std::env::var_os(var).map(PathBuf::from).unwrap_or_else(|| home().join(fallback));
        let builtin = std::env::var_os("PINHOLE_BUILTIN_PLUGINS").filter(|d| !d.is_empty()).map(PathBuf::from).unwrap_or_else(|| {
            let exe = std::env::current_exe().unwrap_or_default();
            let installed = exe.parent().map(|bin| bin.join("../lib/pinhole/plugins")).filter(|d| d.is_dir());
            // core/target/release/pinholed in a checkout: its plugins/.
            installed.or_else(|| exe.ancestors().nth(4).map(|root| root.join("plugins"))).unwrap_or_default()
        });
        Places {
            builtin,
            personal: xdg("XDG_CONFIG_HOME", ".config").join("pinhole/plugins"),
            state: xdg("XDG_STATE_HOME", ".local/state").join("pinhole/sources"),
        }
    }
}

/// A source of a plugin's own, run out of process by its provider: one of
/// its manifest's `sources` with a `provider`.
#[derive(Clone, Debug, PartialEq)]
pub struct Provided {
    /// The plugin's id, whose switch it follows.
    pub plugin: String,
    /// The plugin is a built-in one: on unless switched off. One of your
    /// own is off until switched on.
    pub builtin: bool,
    /// Its kind, which is the plugin's id or starts with it and a dot.
    pub kind: String,
    /// The program, in the plugin's folder.
    pub program: PathBuf,
    /// Its sources can need signing in.
    pub needs_sign_in: bool,
    /// The extensions of the files its sources take (`add`), in lower case.
    pub accepts: Vec<String>,
    /// How often its sources are looked at for changes, if not every minute.
    pub rounds_every: Option<u64>,
}

/// Every provider the plugins declare, and why any declared was refused,
/// one line each. A plugin of your own with a built-in's id takes its
/// place, as in the window. `compiled` are the kinds pinholed has in
/// process, which none may claim.
pub fn provided(places: &Places, compiled: &[&str]) -> (Vec<Provided>, Vec<String>) {
    // (id, folder, manifest, built-in), a later one of an id replacing the
    // one before.
    let mut plugins: Vec<(String, PathBuf, Value, bool)> = Vec::new();
    for (root, builtin) in [(&places.builtin, true), (&places.personal, false)] {
        let Ok(entries) = std::fs::read_dir(root) else { continue };
        let mut dirs: Vec<PathBuf> = entries.flatten().map(|e| e.path()).filter(|p| p.join("manifest.json").is_file()).collect();
        dirs.sort();
        for dir in dirs {
            let manifest = std::fs::read_to_string(dir.join("manifest.json"))
                .ok()
                .and_then(|t| serde_json::from_str::<Value>(&t).ok())
                .unwrap_or(Value::Null);
            let name = dir.file_name().unwrap_or_default().to_string_lossy().into_owned();
            let id = manifest["id"].as_str().map(str::to_owned).unwrap_or(name);
            plugins.retain(|(other, ..)| *other != id);
            plugins.push((id, dir, manifest, builtin));
        }
    }
    let ids: Vec<String> = plugins.iter().map(|(id, ..)| id.clone()).collect();
    let mut found: Vec<Provided> = Vec::new();
    let mut problems = Vec::new();
    for (id, dir, manifest, builtin) in &plugins {
        let Some(sources) = manifest["sources"].as_array() else { continue };
        for declared in sources {
            let Some(provider) = declared.get("provider") else { continue };
            let kind = declared["kind"].as_str().unwrap_or("");
            let builtin = *builtin;
            match check(id, dir, manifest, kind, provider, declared, compiled, &ids, &found) {
                Ok(provided) => found.push(Provided { builtin, ..provided }),
                Err(why) => problems.push(format!("{id}: source {kind:?}: {why}")),
            }
        }
    }
    (found, problems)
}

/// One provider as its manifest declares it, checked (docs/source-providers.md).
#[allow(clippy::too_many_arguments)]
fn check(
    id: &str,
    dir: &Path,
    manifest: &Value,
    kind: &str,
    provider: &Value,
    declared: &Value,
    compiled: &[&str],
    ids: &[String],
    found: &[Provided],
) -> Result<Provided, String> {
    // As the window takes a manifest at all (Plugins::manifestProblem).
    if manifest["schemaVersion"].as_i64() != Some(1) {
        return Err("manifest.json needs \"schemaVersion\": 1".into());
    }
    if id.is_empty() || id.contains('/') || id.contains("..") {
        return Err("manifest.json has no usable \"id\"".into());
    }
    if manifest["api"].as_i64().unwrap_or(1) < 3 {
        return Err("sources need \"api\": 3".into());
    }
    let main = manifest["entryPoints"]["main"].as_str().unwrap_or("");
    if !safe_relative(main) || !dir.join(main).is_file() {
        return Err("manifest.json's \"entryPoints\" has no \"main\" file in the plugin's folder".into());
    }
    let named = kind.len() <= 128
        && kind.starts_with(|c: char| c.is_ascii_alphanumeric())
        && kind.chars().all(|c| c.is_ascii_alphanumeric() || matches!(c, '.' | '_' | '-'));
    if !named {
        return Err("a kind is letters, digits, '.', '_' and '-'".into());
    }
    if kind != id && !kind.strip_prefix(id).is_some_and(|rest| rest.starts_with('.')) {
        return Err(format!("its kind must be the plugin's id, {id}, or start with {id}."));
    }
    if compiled.contains(&kind) {
        return Err("that kind is pinholed's own".into());
    }
    // The kind is the plugin's whose id is the longest that names it: a
    // plugin `me` never takes `me.jpegs` from a plugin `me.jpegs`,
    // whichever is found first.
    if let Some(owner) = owner_of(kind, ids).filter(|owner| *owner != id) {
        return Err(format!("that kind is {owner}'s"));
    }
    if let Some(other) = found.iter().find(|p| p.kind == kind) {
        return Err(format!("{} has that kind already", other.plugin));
    }
    let program = provider.as_str().unwrap_or("");
    if !safe_relative(program) {
        return Err("\"provider\" is a path in the plugin's folder, without \"..\"".into());
    }
    let path = dir.join(program);
    use std::os::unix::fs::PermissionsExt;
    if !std::fs::metadata(&path).is_ok_and(|m| m.is_file() && m.permissions().mode() & 0o111 != 0) {
        return Err(format!("{} is not an executable file", path.display()));
    }
    let accepts: Vec<String> = match &declared["accepts"] {
        Value::Null => Vec::new(),
        Value::Array(list) => list
            .iter()
            .map(|e| {
                e.as_str()
                    .filter(|e| (1..=16).contains(&e.len()) && e.chars().all(|c| c.is_ascii_lowercase() || c.is_ascii_digit()))
                    .map(str::to_owned)
                    .ok_or_else(|| "\"accepts\" holds extensions in lower case, without the dot".to_owned())
            })
            .collect::<Result<_, _>>()?,
        _ => return Err("\"accepts\" is a list of extensions".into()),
    };
    let rounds_every = match &declared["rounds_every_secs"] {
        Value::Null => None,
        v => Some(v.as_u64().filter(|s| (5..=86_400).contains(s)).ok_or("\"rounds_every_secs\" is 5 to 86400")?),
    };
    Ok(Provided {
        plugin: id.to_owned(),
        builtin: false,
        kind: kind.to_owned(),
        program: path,
        needs_sign_in: declared["needs_sign_in"].as_bool().unwrap_or(false),
        accepts,
        rounds_every,
    })
}

/// The plugin a kind belongs to, of those installed: the one with the
/// longest id that is the kind, or that it starts with and a dot.
fn owner_of<'a>(kind: &str, ids: &'a [String]) -> Option<&'a str> {
    ids.iter()
        .filter(|id| kind == id.as_str() || kind.strip_prefix(id.as_str()).is_some_and(|rest| rest.starts_with('.')))
        .max_by_key(|id| id.len())
        .map(String::as_str)
}

/// A path inside a plugin's folder: relative, and never out of it.
fn safe_relative(path: &str) -> bool {
    !path.is_empty()
        && Path::new(path).components().all(|c| matches!(c, std::path::Component::Normal(_) | std::path::Component::CurDir))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn switches_are_what_enabled_says_and_nothing_else() {
        let dir = std::env::temp_dir().join(format!("pinhole-plugins-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        let path = dir.join("plugins.json");
        // No file: nothing turned.
        assert!(switches_in(&path).is_empty());
        assert_eq!(stamp(&path), None);
        // Not JSON, or JSON of another shape: nothing turned.
        for text in ["{not json", "[1, 2]", r#"{"enabled": [true]}"#, r#"{"settings": {}, "toured": ["x"]}"#] {
            std::fs::write(&path, text).unwrap();
            assert!(switches_in(&path).is_empty(), "{text}");
        }
        // Off and on as written; anything but a boolean is no switch.
        std::fs::write(&path, r#"{"enabled": {"pinhole.series": false, "pinhole.folder": true, "odd": "yes"}, "toured": []}"#)
            .unwrap();
        let s = switches_in(&path);
        assert_eq!(s.get("pinhole.series"), Some(&false));
        assert_eq!(s.get("pinhole.folder"), Some(&true));
        assert_eq!(s.len(), 2);
        assert!(stamp(&path).is_some());
        // Caught half written, or broken by hand: the switches stay as they
        // were, and a plugin turned off is not turned on by it.
        for broken in [r#"{"enabled": {"pinhole.series": fal"#, "", "[]"] {
            std::fs::write(&path, broken).unwrap();
            assert_eq!(switches_in(&path), s, "{broken:?}");
        }
        std::fs::write(&path, r#"{"enabled": {"pinhole.series": true}}"#).unwrap();
        assert_eq!(switches_in(&path).get("pinhole.series"), Some(&true));
        // No file at all: nothing turned.
        std::fs::remove_file(&path).unwrap();
        assert!(switches_in(&path).is_empty());
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn a_kind_is_the_plugins_with_the_longest_id_that_names_it_whatever_the_folders_order() {
        use std::os::unix::fs::PermissionsExt;
        let dir = std::env::temp_dir().join(format!("pinhole-owners-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        let places = Places { builtin: dir.join("builtin"), personal: dir.join("personal"), state: dir.join("state") };
        let plugin = |folder: &str, id: &str, kinds: &[&str]| {
            let at = places.personal.join(folder);
            std::fs::create_dir_all(&at).unwrap();
            std::fs::write(at.join("Main.qml"), "import Pinhole.Plugin\nPinholePlugin {}\n").unwrap();
            let sources: Vec<Value> = kinds.iter().map(|k| serde_json::json!({"kind": k, "provider": "run"})).collect();
            let manifest = serde_json::json!({"schemaVersion": 1, "api": 3, "id": id, "entryPoints": {"main": "Main.qml"}, "sources": sources});
            std::fs::write(at.join("manifest.json"), manifest.to_string()).unwrap();
            std::fs::write(at.join("run"), "#!/bin/sh\n").unwrap();
            std::fs::set_permissions(at.join("run"), std::fs::Permissions::from_mode(0o755)).unwrap();
        };
        // `me`, found first, claims what `me.jpegs` and `me.jpegs.raw`
        // name; `me.jpegs` claims `me.jpegs.raw`, which is another's too.
        plugin("a-me", "me", &["me", "me.jpegs", "me.jpegs.raw", "me.jpegsx"]);
        plugin("jpegs", "me.jpegs", &["me.jpegs", "me.jpegs.raw", "me.jpegs.small"]);
        plugin("raw", "me.jpegs.raw", &[]);
        let (found, problems) = provided(&places, &[]);
        let kinds: Vec<(&str, &str)> = found.iter().map(|p| (p.plugin.as_str(), p.kind.as_str())).collect();
        assert_eq!(kinds, [("me", "me"), ("me", "me.jpegsx"), ("me.jpegs", "me.jpegs"), ("me.jpegs", "me.jpegs.small")]);
        let said = problems.join("\n");
        for why in ["me: source \"me.jpegs\": that kind is me.jpegs's", "me: source \"me.jpegs.raw\": that kind is me.jpegs.raw's",
                    "me.jpegs: source \"me.jpegs.raw\": that kind is me.jpegs.raw's"] {
            assert!(said.contains(why), "{why} not in:\n{said}");
        }
        assert_eq!(problems.len(), 3, "{said}");
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn providers_are_found_and_checked_as_their_manifests_say() {
        use std::os::unix::fs::PermissionsExt;
        let dir = std::env::temp_dir().join(format!("pinhole-provided-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        let places = Places { builtin: dir.join("builtin"), personal: dir.join("personal"), state: dir.join("state") };
        let plugin = |root: &Path, folder: &str, manifest: Value, program: Option<(&str, u32)>| {
            let at = root.join(folder);
            std::fs::create_dir_all(&at).unwrap();
            std::fs::write(at.join("Main.qml"), "import Pinhole.Plugin\nPinholePlugin {}\n").unwrap();
            std::fs::write(at.join("manifest.json"), manifest.to_string()).unwrap();
            if let Some((name, mode)) = program {
                std::fs::write(at.join(name), "#!/bin/sh\n").unwrap();
                std::fs::set_permissions(at.join(name), std::fs::Permissions::from_mode(mode)).unwrap();
            }
        };
        let manifest = |id: &str, sources: Value| {
            serde_json::json!({"schemaVersion": 1, "api": 3, "id": id, "entryPoints": {"main": "Main.qml"}, "sources": sources})
        };
        let one = |kind: &str, provider: &str| serde_json::json!([{"kind": kind, "provider": provider}]);
        // A built-in with an in-process kind (no provider: not one of these),
        // and one with a provider, which a personal plugin of its id replaces.
        plugin(&places.builtin, "icloud", manifest("pinhole.icloud", serde_json::json!([{"kind": "icloud"}])), None);
        plugin(&places.builtin, "nas", manifest("pinhole.nas", one("pinhole.nas", "bin/nas")), None);
        std::fs::create_dir_all(places.builtin.join("nas/bin")).unwrap();
        std::fs::write(places.builtin.join("nas/bin/nas"), "#!/bin/sh\n").unwrap();
        std::fs::set_permissions(places.builtin.join("nas/bin/nas"), std::fs::Permissions::from_mode(0o755)).unwrap();
        plugin(&places.builtin, "photos", manifest("pinhole.photos", one("pinhole.photos", "p")), Some(("p", 0o755)));
        plugin(&places.personal, "my-photos", manifest("pinhole.photos", serde_json::json!([])), None);
        // Of your own: one right, with what it says of itself, and many wrong.
        plugin(&places.personal, "immich", manifest("me.immich", serde_json::json!([
            {"kind": "me.immich", "provider": "./run", "needs_sign_in": true, "accepts": ["jpg", "heic"], "rounds_every_secs": 300},
            {"kind": "me.immich.shared", "provider": "run"},
            {"kind": "me.immich", "provider": "run"},
            {"kind": "me.immichx", "provider": "run"},
            {"kind": "icloud", "provider": "run"},
            {"kind": "me.immich/../x", "provider": "run"},
            {"kind": "me.immich.up", "provider": "../run"},
            {"kind": "me.immich.abs", "provider": "/bin/sh"},
            {"kind": "me.immich.missing", "provider": "nothing"},
            {"kind": "me.immich.accepts", "provider": "run", "accepts": [".JPG"]},
            {"kind": "me.immich.rounds", "provider": "run", "rounds_every_secs": 1},
        ])), Some(("run", 0o755)));
        plugin(&places.personal, "plain", manifest("me.plain", one("me.plain", "run")), Some(("run", 0o644)));
        plugin(&places.personal, "old", serde_json::json!({"schemaVersion": 1, "id": "me.old", "entryPoints": {"main": "Main.qml"},
                                                           "sources": one("me.old", "run")}), Some(("run", 0o755)));
        plugin(&places.personal, "icloud-twin", manifest("icloud", one("icloud", "run")), Some(("run", 0o755)));

        let (found, problems) = provided(&places, &["icloud", "folder"]);
        let kinds: Vec<(&str, &str, bool)> = found.iter().map(|p| (p.plugin.as_str(), p.kind.as_str(), p.builtin)).collect();
        assert_eq!(kinds, [("pinhole.nas", "pinhole.nas", true), ("me.immich", "me.immich", false), ("me.immich", "me.immich.shared", false)]);
        assert_eq!(found[0].program, places.builtin.join("nas/bin/nas"));
        assert_eq!((found[1].needs_sign_in, found[1].accepts.clone(), found[1].rounds_every), (true, vec!["jpg".to_owned(), "heic".to_owned()], Some(300)));
        assert_eq!((found[2].needs_sign_in, found[2].accepts.len(), found[2].rounds_every), (false, 0, None));
        let said = problems.join("\n");
        for why in ["\"me.immich\": me.immich has that kind already", "\"me.immichx\": its kind must be", "\"icloud\": its kind must be",
                    "\"me.immich/../x\": a kind is letters", "\"me.immich.up\": \"provider\" is a path", "\"me.immich.abs\": \"provider\" is a path",
                    "\"me.immich.missing\":", "\"me.immich.accepts\": \"accepts\"", "\"me.immich.rounds\": \"rounds_every_secs\"",
                    "me.plain: source \"me.plain\":", "me.old: source \"me.old\": sources need \"api\": 3",
                    "icloud: source \"icloud\": that kind is pinholed's own"] {
            assert!(said.contains(why), "{why} not in:\n{said}");
        }
        assert_eq!(problems.len(), 12, "{said}");
        let _ = std::fs::remove_dir_all(&dir);
    }
}
