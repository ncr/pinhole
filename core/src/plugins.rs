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

/// The ids of the plugins of your own (`personal`), each by its manifest
/// (its folder's name when that says none). One with a built-in plugin's
/// id is your own copy of it, which the window keeps off until you turn it
/// on: so does pinholed the built-in kinds of that id (`Kinds::refresh`).
pub fn personal_ids(places: &Places) -> std::collections::HashSet<String> {
    let Ok(entries) = std::fs::read_dir(&places.personal) else { return Default::default() };
    entries
        .flatten()
        .map(|e| e.path())
        .filter(|dir| dir.join("manifest.json").is_file())
        .map(|dir| {
            let manifest = std::fs::read_to_string(dir.join("manifest.json"))
                .ok()
                .and_then(|t| serde_json::from_str::<Value>(&t).ok())
                .unwrap_or(Value::Null);
            manifest["id"].as_str().map(str::to_owned).unwrap_or_else(|| dir.file_name().unwrap_or_default().to_string_lossy().into_owned())
        })
        .collect()
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

/// Every plugin in `places`: (id, folder, manifest, built-in), sorted by
/// folder, a plugin of your own with a built-in's id in that one's place,
/// as in the window.
fn installed(places: &Places) -> Vec<(String, PathBuf, Value, bool)> {
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
    plugins
}

/// Whether a plugin is on, by its switch: a built-in one unless switched
/// off, one of your own (your copy of a built-in too) only when switched
/// on, as the window has them.
pub fn turned_on(builtin: bool, switch: Option<bool>) -> bool {
    if builtin { switch != Some(false) } else { switch == Some(true) }
}

/// Whether the plugin `id` is on, by its switch in `switches`. Your own
/// copy of a built-in (`copy`) has a switch of its own, `copy:<id>`: the
/// built-in's, turned on once, never turns on a copy half made
/// (Plugins::switchKey in the window, the same).
pub fn is_on(switches: &HashMap<String, bool>, id: &str, builtin: bool, copy: bool) -> bool {
    let switch = if copy { switches.get(&format!("copy:{id}")) } else { switches.get(id) };
    turned_on(builtin && !copy, switch.copied())
}

/// The ids of Pinhole's own plugins (`builtin`).
pub fn builtin_ids(places: &Places) -> std::collections::HashSet<String> {
    let Ok(entries) = std::fs::read_dir(&places.builtin) else { return Default::default() };
    entries
        .flatten()
        .map(|e| e.path())
        .filter(|dir| dir.join("manifest.json").is_file())
        .map(|dir| {
            let manifest = std::fs::read_to_string(dir.join("manifest.json"))
                .ok()
                .and_then(|t| serde_json::from_str::<Value>(&t).ok())
                .unwrap_or(Value::Null);
            manifest["id"].as_str().map(str::to_owned).unwrap_or_else(|| dir.file_name().unwrap_or_default().to_string_lossy().into_owned())
        })
        .collect()
}

/// Every provider the plugins declare, and why any declared was refused,
/// one line each. A plugin of your own with a built-in's id takes its
/// place, as in the window. `compiled` are the kinds pinholed has in
/// process, which none may claim.
pub fn provided(places: &Places, compiled: &[&str]) -> (Vec<Provided>, Vec<String>) {
    let plugins = installed(places);
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

/// A program of a plugin's that pinholed keeps running for the source
/// shown, while the plugin is on: one of its manifest's `workers`
/// (docs/plugins.md, Workers).
#[derive(Clone, Debug, PartialEq)]
pub struct Worker {
    /// The plugin's id, whose switch it follows.
    pub plugin: String,
    /// The plugin is a built-in one: on unless switched off.
    pub builtin: bool,
    /// Its name, one of the plugin's own.
    pub name: String,
    /// The program, in the plugin's folder.
    pub program: PathBuf,
    /// What it is given before what pinholed gives every worker.
    pub args: Vec<String>,
}

/// The plugin API that brought `workers` (Plugins::apiUses).
pub const WORKERS_API: i64 = 11;

/// Every worker the plugins declare, on or not, and why any declared was
/// refused, one line each. A plugin of your own with a built-in's id
/// takes its place, its workers too.
pub fn workers(places: &Places) -> (Vec<Worker>, Vec<String>) {
    let mut found = Vec::new();
    let mut problems = Vec::new();
    for (id, dir, manifest, builtin) in installed(places) {
        let declared = &manifest["workers"];
        if declared.is_null() {
            continue;
        }
        let Some(declared) = declared.as_array() else {
            problems.push(format!("{id}: \"workers\" is a list"));
            continue;
        };
        if let Err(why) = taken(&id, &dir, &manifest).and_then(|()| {
            if whole(&manifest["api"]).unwrap_or(1) < WORKERS_API {
                return Err(format!("workers need \"api\": {WORKERS_API}"));
            }
            Ok(())
        }) {
            problems.push(format!("{id}: workers: {why}"));
            continue;
        }
        let mut names: Vec<String> = Vec::new();
        for each in declared {
            let name = each["name"].as_str().unwrap_or("");
            match check_worker(&dir, each, &names) {
                Ok((program, args)) => {
                    names.push(name.to_owned());
                    found.push(Worker { plugin: id.clone(), builtin, name: name.to_owned(), program, args });
                }
                Err(why) => problems.push(format!("{id}: worker {name:?}: {why}")),
            }
        }
    }
    (found, problems)
}

/// The API from which a plugin can make renditions (`makes`).
pub const MAKES_API: i64 = 12;

/// A rendition a plugin makes from another of a photo's, while someone
/// waits for it (a Live Photo's Bounce from its motion): its manifest's
/// `makes`.
#[derive(Debug, Clone)]
pub struct Maker {
    pub plugin: String,
    pub builtin: bool,
    /// The kind a window asks `fetch` for: `bounce`, `long_exposure`.
    pub kind: String,
    /// The rendition it is made from: `live`, `full`, `original`, `video`.
    pub from: String,
    /// What it is: its UTI, and its files' extension.
    pub file_type: String,
    pub extension: String,
    pub program: PathBuf,
    pub args: Vec<String>,
}

/// Every rendition the plugins declare they make, on or not, and why any
/// declared was refused. A plugin of your own with a built-in's id takes
/// its place, its makers too.
pub fn makers(places: &Places) -> (Vec<Maker>, Vec<String>) {
    let mut found: Vec<Maker> = Vec::new();
    let mut problems = Vec::new();
    for (id, dir, manifest, builtin) in installed(places) {
        let declared = &manifest["makes"];
        if declared.is_null() {
            continue;
        }
        let Some(declared) = declared.as_array() else {
            problems.push(format!("{id}: \"makes\" is a list"));
            continue;
        };
        if let Err(why) = taken(&id, &dir, &manifest).and_then(|()| {
            if whole(&manifest["api"]).unwrap_or(1) < MAKES_API {
                return Err(format!("makes needs \"api\": {MAKES_API}"));
            }
            Ok(())
        }) {
            problems.push(format!("{id}: makes: {why}"));
            continue;
        }
        for each in declared {
            let kind = each["kind"].as_str().unwrap_or("");
            match check_maker(&id, whole(&manifest["api"]).unwrap_or(1), &dir, each) {
                Ok(maker) if found.iter().any(|m| m.kind == maker.kind) => {
                    problems.push(format!("{id}: makes {kind:?}: another plugin makes it already"))
                }
                Ok(maker) => found.push(Maker { plugin: id.clone(), builtin, ..maker }),
                Err(why) => problems.push(format!("{id}: makes {kind:?}: {why}")),
            }
        }
    }
    (found, problems)
}

/// The kinds a plugin can make: the effects a source may have no render
/// of. What the window asks for by these names it makes; nothing else.
pub const MADE_KINDS: [&str; 2] = ["bounce", "long_exposure"];

/// The API from which a plugin can make kinds of its own (`makes`).
pub const OWN_KINDS_API: i64 = 15;

/// A kind a plugin makes of its own: `<its id>/<name>`, the name in
/// lowercase letters, digits, '_' and '-'. Its id in front keeps one
/// plugin's from another's, and from the kinds a source has.
pub fn own_kind<'a>(plugin: &str, kind: &'a str) -> Option<&'a str> {
    let name = kind.strip_prefix(plugin)?.strip_prefix('/')?;
    let fine = (1..=32).contains(&name.len())
        && name.starts_with(|c: char| c.is_ascii_lowercase() || c.is_ascii_digit())
        && name.chars().all(|c| c.is_ascii_lowercase() || c.is_ascii_digit() || matches!(c, '_' | '-'));
    fine.then_some(name)
}

/// Whether a kind asked of `fetch` is a plugin's own (it has a '/').
pub fn is_own_kind(kind: &str) -> bool {
    kind.contains('/')
}

/// One rendition a manifest says it makes, checked.
fn check_maker(plugin: &str, api: i64, dir: &Path, declared: &Value) -> Result<Maker, String> {
    if !declared.is_object() {
        return Err("each is an object: \"kind\", \"from\", \"type\", \"extension\", \"program\"".into());
    }
    let kind = declared["kind"].as_str().unwrap_or("");
    if is_own_kind(kind) {
        if own_kind(plugin, kind).is_none() {
            return Err(format!("a kind of its own is \"{plugin}/<name>\", the name lowercase letters, digits, '_' and '-'"));
        }
        if api < OWN_KINDS_API {
            return Err(format!("a kind of its own needs \"api\": {OWN_KINDS_API}"));
        }
    } else if !MADE_KINDS.contains(&kind) {
        return Err(format!("\"kind\" is one of {}, or one of its own: \"{plugin}/<name>\"", MADE_KINDS.join(", ")));
    }
    let from = declared["from"].as_str().unwrap_or("");
    if !["live", "full", "original", "video"].contains(&from) {
        return Err("\"from\" is live, full, original or video".into());
    }
    let file_type = declared["type"].as_str().unwrap_or("");
    if file_type.is_empty() || file_type.len() > 128 || !file_type.chars().all(|c| c.is_ascii_alphanumeric() || matches!(c, '.' | '-')) {
        return Err("\"type\" is its UTI, public.jpeg say".into());
    }
    let extension = declared["extension"].as_str().unwrap_or("");
    if extension.is_empty() || extension.len() > 8 || !extension.chars().all(|c| c.is_ascii_lowercase() || c.is_ascii_digit()) {
        return Err("\"extension\" is its files', lowercase: jpg, mp4".into());
    }
    let args = match &declared["args"] {
        Value::Null => Vec::new(),
        Value::Array(list) if list.len() <= 64 => list
            .iter()
            .map(|a| a.as_str().filter(|a| a.len() <= 4096).map(str::to_owned).ok_or_else(|| "\"args\" is a list of strings".to_owned()))
            .collect::<Result<_, _>>()?,
        _ => return Err("\"args\" is a list of strings".into()),
    };
    let program = program_in(dir, "program", &declared["program"])?;
    Ok(Maker {
        plugin: String::new(),
        builtin: false,
        kind: kind.to_owned(),
        from: from.to_owned(),
        file_type: file_type.to_owned(),
        extension: extension.to_owned(),
        program,
        args,
    })
}

/// One worker as its manifest declares it, checked: its program and the
/// arguments it is given first.
fn check_worker(dir: &Path, declared: &Value, names: &[String]) -> Result<(PathBuf, Vec<String>), String> {
    if !declared.is_object() {
        return Err("a worker is an object with a \"name\" and a \"program\"".into());
    }
    let name = declared["name"].as_str().unwrap_or("");
    let named = name.len() <= 64
        && name.starts_with(|c: char| c.is_ascii_alphanumeric())
        && name.chars().all(|c| c.is_ascii_alphanumeric() || matches!(c, '.' | '_' | '-'));
    if !named {
        return Err("a worker's \"name\" is letters, digits, '.', '_' and '-'".into());
    }
    if names.iter().any(|n| n == name) {
        return Err("that name is the plugin's already".into());
    }
    match &declared["per"] {
        Value::Null => {}
        Value::String(per) if per == "source" => {}
        _ => return Err("\"per\" is \"source\"".into()),
    }
    let args = match &declared["args"] {
        Value::Null => Vec::new(),
        Value::Array(list) if list.len() <= 64 => list
            .iter()
            .map(|a| a.as_str().filter(|a| a.len() <= 4096).map(str::to_owned).ok_or_else(|| "\"args\" is a list of strings".to_owned()))
            .collect::<Result<_, _>>()?,
        _ => return Err("\"args\" is a list of strings".into()),
    };
    let program = program_in(dir, "program", &declared["program"])?;
    Ok((program, args))
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
    taken(id, dir, manifest)?;
    if whole(&manifest["api"]).unwrap_or(1) < 3 {
        return Err("sources need \"api\": 3".into());
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
    let path = program_in(dir, "provider", provider)?;
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
        v => Some(whole(v).and_then(|s| u64::try_from(s).ok()).filter(|s| (5..=86_400).contains(s)).ok_or("\"rounds_every_secs\" is 5 to 86400")?),
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

/// A whole number, written `11` or `11.0` (as the window's Qt reads both).
fn whole(value: &Value) -> Option<i64> {
    value.as_i64().or_else(|| value.as_f64().filter(|f| f.is_finite() && f.fract() == 0.0 && f.abs() < 1e15).map(|f| f as i64))
}

/// An id a plugin can have: what its folders and files are named after
/// (state, cache, logs), so letters, digits, `.`, `_` and `-`, beginning
/// with a letter or digit, and no `..` (Plugins::usableId, the same).
pub fn usable_id(id: &str) -> bool {
    !id.is_empty()
        && id.len() <= 128
        && id.starts_with(|c: char| c.is_ascii_alphanumeric())
        && id.chars().all(|c| c.is_ascii_alphanumeric() || matches!(c, '.' | '_' | '-'))
        && !id.contains("..")
}

/// As the window takes a manifest at all (Plugins::manifestProblem).
fn taken(id: &str, dir: &Path, manifest: &Value) -> Result<(), String> {
    if whole(&manifest["schemaVersion"]) != Some(1) {
        return Err("manifest.json needs \"schemaVersion\": 1".into());
    }
    if !usable_id(id) {
        return Err("manifest.json has no usable \"id\"".into());
    }
    let main = manifest["entryPoints"]["main"].as_str().unwrap_or("");
    if !safe_relative(main) || !dir.join(main).is_file() {
        return Err("manifest.json's \"entryPoints\" has no \"main\" file in the plugin's folder".into());
    }
    Ok(())
}

/// A program a manifest names (its `field`): a path in the plugin's
/// folder, never out of it, to a file that can be run.
fn program_in(dir: &Path, field: &str, named: &Value) -> Result<PathBuf, String> {
    let program = named.as_str().unwrap_or("");
    if !safe_relative(program) {
        return Err(format!("\"{field}\" is a path in the plugin's folder, without \"..\""));
    }
    let path = dir.join(program);
    use std::os::unix::fs::PermissionsExt;
    if !std::fs::metadata(&path).is_ok_and(|m| m.is_file() && m.permissions().mode() & 0o111 != 0) {
        return Err(format!("{} is not an executable file", path.display()));
    }
    Ok(path)
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
    fn what_a_plugin_makes_is_read_from_its_manifest_and_checked() {
        let dir = std::env::temp_dir().join(format!("pinhole-makers-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        let places = Places { builtin: dir.join("builtin"), personal: dir.join("personal"), state: dir.join("state") };
        let plugin = |root: &Path, folder: &str, manifest: Value| {
            let at = root.join(folder);
            std::fs::create_dir_all(&at).unwrap();
            std::fs::write(at.join("Main.qml"), "").unwrap();
            std::fs::write(at.join("make"), "#!/bin/sh\n").unwrap();
            std::fs::set_permissions(at.join("make"), std::os::unix::fs::PermissionsExt::from_mode(0o755)).unwrap();
            std::fs::write(at.join("manifest.json"), manifest.to_string()).unwrap();
        };
        let made = |kind: &str| serde_json::json!({"kind": kind, "from": "live", "type": "public.mpeg-4", "extension": "mp4",
                                                   "program": "make", "args": [kind]});
        let manifest = |id: &str, api: i64, makes: Value| serde_json::json!({"schemaVersion": 1, "api": api, "id": id,
                                                                              "entryPoints": {"main": "Main.qml"}, "makes": makes});
        plugin(&places.builtin, "live", manifest("pinhole.live", 12, serde_json::json!([made("bounce"), made("long_exposure")])));
        plugin(&places.personal, "old", manifest("me.old", 11, serde_json::json!([made("bounce")])));
        plugin(&places.personal, "odd", manifest("me.odd", 12, serde_json::json!([made("sparkle"),
            {"kind": "bounce", "from": "live", "type": "public.mpeg-4", "extension": "MP4", "program": "make"}])));
        // Kinds of its own: under its id, from API 15.
        plugin(&places.personal, "steady", manifest("me.steady", 15, serde_json::json!([made("me.steady/steadied"),
            made("me.other/steadied"), made("me.steady/Not Fine")])));
        plugin(&places.personal, "early", manifest("me.early", 14, serde_json::json!([made("me.early/sooner")])));
        let (found, refused) = makers(&places);
        let kinds: Vec<(&str, &str)> = found.iter().map(|m| (m.plugin.as_str(), m.kind.as_str())).collect();
        assert_eq!(kinds, [("pinhole.live", "bounce"), ("pinhole.live", "long_exposure"), ("me.steady", "me.steady/steadied")]);
        assert!(found[0].builtin && found[0].program.ends_with("live/make") && found[0].args == ["bounce"]);
        let refused = refused.join("\n");
        assert!(refused.contains("me.old: makes: makes needs \"api\": 12"), "{refused}");
        assert!(refused.contains("me.odd: makes \"sparkle\": \"kind\" is one of bounce, long_exposure"), "{refused}");
        assert!(refused.contains("me.steady: makes \"me.other/steadied\": a kind of its own is \"me.steady/<name>\""), "{refused}");
        assert!(refused.contains("me.steady: makes \"me.steady/Not Fine\""), "{refused}");
        assert!(refused.contains("me.early: makes \"me.early/sooner\": a kind of its own needs \"api\": 15"), "{refused}");
        assert!(refused.contains("me.odd: makes \"bounce\": \"extension\""), "{refused}");
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn an_id_names_folders_safely_and_numbers_may_be_written_as_decimals() {
        for id in ["pinhole.series", "me.night", "t", "a_b-c.1"] {
            assert!(usable_id(id), "{id}");
        }
        for id in ["", ".", "..", "a/b", "a..b", ".hidden", "my plugin", "x\nY", "pinhole.\u{e9}"] {
            assert!(!usable_id(id), "{id:?}");
        }
        assert_eq!(whole(&serde_json::json!(11)), Some(11));
        assert_eq!(whole(&serde_json::json!(11.0)), Some(11));
        assert_eq!(whole(&serde_json::json!(11.5)), None);
        assert_eq!(whole(&serde_json::json!("11")), None);
    }

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

    #[test]
    fn workers_are_found_and_checked_as_their_manifests_say() {
        use std::os::unix::fs::PermissionsExt;
        let dir = std::env::temp_dir().join(format!("pinhole-workers-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        let places = Places { builtin: dir.join("builtin"), personal: dir.join("personal"), state: dir.join("state") };
        let plugin = |root: &Path, folder: &str, id: &str, api: i64, workers: Value| {
            let at = root.join(folder);
            std::fs::create_dir_all(&at).unwrap();
            std::fs::write(at.join("Main.qml"), "import Pinhole.Plugin\nPinholePlugin {}\n").unwrap();
            let manifest = serde_json::json!({"schemaVersion": 1, "api": api, "id": id, "entryPoints": {"main": "Main.qml"}, "workers": workers});
            std::fs::write(at.join("manifest.json"), manifest.to_string()).unwrap();
            for (name, mode) in [("run", 0o755), ("plain", 0o644)] {
                std::fs::write(at.join(name), "#!/bin/sh\n").unwrap();
                std::fs::set_permissions(at.join(name), std::fs::Permissions::from_mode(mode)).unwrap();
            }
        };
        plugin(&places.builtin, "people", "pinhole.people", 11,
               serde_json::json!([{"name": "faces", "program": "run", "args": ["daemon"], "per": "source"}]));
        // A built-in one your copy replaces, workers and all.
        plugin(&places.builtin, "search", "pinhole.search", 11, serde_json::json!([{"name": "old", "program": "run"}]));
        plugin(&places.personal, "my-search", "pinhole.search", 11, serde_json::json!([{"name": "new", "program": "./run"}]));
        // Of your own: one right, and many wrong.
        plugin(&places.personal, "mine", "me.mine", 11, serde_json::json!([
            {"name": "index", "program": "run"},
            {"name": "index", "program": "run"},
            {"name": "up", "program": "../mine/run"},
            {"name": "abs", "program": "/bin/sh"},
            {"name": "plain", "program": "plain"},
            {"name": "missing", "program": "nothing"},
            {"name": "bad/name", "program": "run"},
            {"name": "per", "program": "run", "per": "photo"},
            {"name": "args", "program": "run", "args": ["a", 1]},
            "run",
        ]));
        plugin(&places.personal, "old", "me.old", 10, serde_json::json!([{"name": "w", "program": "run"}]));
        plugin(&places.personal, "odd", "me.odd", 11, serde_json::json!({"name": "w", "program": "run"}));

        let (found, problems) = workers(&places);
        let names: Vec<(&str, &str, bool)> = found.iter().map(|w| (w.plugin.as_str(), w.name.as_str(), w.builtin)).collect();
        assert_eq!(names, [("pinhole.people", "faces", true), ("me.mine", "index", false), ("pinhole.search", "new", false)]);
        assert_eq!(found[0].program, places.builtin.join("people/run"));
        assert_eq!(found[0].args, ["daemon"]);
        assert_eq!(found[2].program, places.personal.join("my-search/./run"));
        let said = problems.join("\n");
        for why in ["me.mine: worker \"index\": that name is the plugin's already", "me.mine: worker \"up\": \"program\" is a path",
                    "me.mine: worker \"abs\": \"program\" is a path", "me.mine: worker \"plain\":", "me.mine: worker \"missing\":",
                    "me.mine: worker \"bad/name\": a worker's \"name\"", "me.mine: worker \"per\": \"per\" is \"source\"",
                    "me.mine: worker \"args\": \"args\" is a list of strings", "me.mine: worker \"\": a worker is an object",
                    "me.old: workers: workers need \"api\": 11", "me.odd: \"workers\" is a list"] {
            assert!(said.contains(why), "{why} not in:\n{said}");
        }
        assert_eq!(problems.len(), 11, "{said}");
        // On as the window has them: a built-in unless turned off, one of
        // your own (your copy of a built-in too) only when turned on.
        assert!(turned_on(true, None) && turned_on(true, Some(true)) && !turned_on(true, Some(false)));
        assert!(!turned_on(false, None) && turned_on(false, Some(true)) && !turned_on(false, Some(false)));
        // A copy of a built-in: its own switch, never the built-in's.
        let switches: HashMap<String, bool> = [("pinhole.icloud".to_owned(), true)].into();
        assert!(is_on(&switches, "pinhole.icloud", true, false));
        assert!(!is_on(&switches, "pinhole.icloud", false, true));
        let switches: HashMap<String, bool> = [("copy:pinhole.icloud".to_owned(), true)].into();
        assert!(is_on(&switches, "pinhole.icloud", false, true));
        let _ = std::fs::remove_dir_all(&dir);
    }
}
