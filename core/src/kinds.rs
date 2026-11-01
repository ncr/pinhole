//! Source kinds: what pinholed knows of each kind of source apart from the
//! `Source` it makes (docs/core-and-plugins.md, "Source kinds"). A kind
//! says how its settings entries look and what their source id is, makes
//! the source, describes it for the window, adds one, and signs in and out
//! where that means anything. The service names no kind itself.
//!
//! Each kind follows the switch of the plugin that owns it in
//! `plugins.json` (plugins.rs): `icloud` is `pinhole.icloud`'s, `folder`
//! `pinhole.folder`'s, tied here and never claimed by a manifest. A kind
//! that is off keeps its entries in the settings and its libraries on disk;
//! it is only not listed, not shown and not synced, until it is on again.

use std::collections::{HashMap, HashSet};
use std::path::PathBuf;
use std::sync::{Arc, RwLock};

use anyhow::{anyhow, Result};
use serde_json::{json, Value};

use crate::source::{NoSource, Source};
use crate::storage::Settings;

pub trait Kind: Send + Sync {
    /// As settings.json names it: "icloud", "folder".
    fn kind(&self) -> &str;

    /// The plugin whose switch it follows: "pinhole.icloud".
    fn plugin(&self) -> &str;

    /// An entry of this kind in its usual form, or None for one that
    /// names nothing.
    fn normal(&self, entry: &Value) -> Option<Value>;

    /// The entry's source id, as its source's `id()` says it:
    /// "icloud:<apple id>", "folder:<path>".
    fn id_of(&self, entry: &Value) -> String;

    /// Entries known without the settings naming them: the Apple IDs
    /// signed in to here.
    fn implicit(&self) -> Vec<Value> {
        Vec::new()
    }

    /// Whether, with no list in the settings, its sources are only its
    /// `implicit` ones: one shown that is not among them is not added (an
    /// Apple ID not signed in to). A kind with none (a folder) has the one
    /// shown added after them.
    fn implicit_only(&self) -> bool {
        false
    }

    fn source(&self, entry: &Value) -> Result<Arc<dyn Source>>;

    /// The `sources` event's own fields for an entry: `name`, and
    /// `account` or `path`.
    fn describe(&self, entry: &Value) -> Value;

    /// `set_source` with this kind: the entry for these params, checked.
    fn entry_for(&self, params: &Value) -> Result<Value, Failure>;

    /// What could be added, given those added already (of every kind):
    /// the window gets it as `discover`'s `kinds[<kind>]`.
    fn discover(&self, _added: &[Value]) -> Value {
        Value::Null
    }

    /// Whether its sources can need signing in.
    fn needs_sign_in(&self) -> bool {
        false
    }

    /// The extensions of the files `add` takes for a source of it.
    fn accepts(&self) -> &'static [&'static str] {
        &[]
    }

    fn sign_in(&self, _params: &Value) -> Result<SignedIn, Failure> {
        Err(Failure::unsupported(format!("{} is not signed in to", self.kind())))
    }

    fn submit_code(&self, _params: &Value) -> Result<SignedIn, Failure> {
        Err(Failure::unsupported(format!("{} is not signed in to", self.kind())))
    }

    /// Signed out of: it stays among the sources, with its library.
    fn sign_out(&self, entry: &Value) -> Result<(), Failure> {
        Err(Failure::bad_request(format!("{} is not signed in to", self.id_of(entry))))
    }

    /// Taken out of the sources: what it kept apart goes (an Apple ID's
    /// session). Its library is the service's to delete.
    fn removed(&self, _entry: &Value) {}
}

/// How a sign-in went: done, with the entry to add and show, or waiting
/// for a code.
#[derive(Debug, PartialEq)]
pub enum SignedIn {
    Entry(Value),
    NeedsCode,
}

/// A request a kind could not do: the window's error code, and why.
#[derive(Debug, Clone, PartialEq)]
pub struct Failure {
    pub code: String,
    pub message: String,
}

impl Failure {
    pub fn new(code: &str, message: impl Into<String>) -> Failure {
        Failure { code: code.to_owned(), message: message.into() }
    }

    pub fn bad_request(message: impl Into<String>) -> Failure {
        Failure::new("bad_request", message)
    }

    pub fn unsupported(message: impl Into<String>) -> Failure {
        Failure::new("unsupported", message)
    }
}

impl From<Failure> for (String, String) {
    fn from(f: Failure) -> Self {
        (f.code, f.message)
    }
}

/// Every kind pinholed knows, and which are on.
pub struct Kinds {
    all: Vec<Arc<dyn Kind>>,
    on: RwLock<HashSet<String>>,
}

impl Kinds {
    /// These kinds, in this order (the order sources are implied in), all
    /// on until `refresh` says otherwise.
    pub fn new(all: Vec<Arc<dyn Kind>>) -> Kinds {
        let on = all.iter().map(|k| k.kind().to_owned()).collect();
        Kinds { all, on: RwLock::new(on) }
    }

    /// The kinds compiled into pinholed: iCloud first, as Apple IDs have
    /// always been the first sources, then folders. `auth` is the sign-in
    /// provider.
    pub fn builtin(auth: PathBuf) -> Kinds {
        Kinds::new(vec![Arc::new(crate::icloud::kind::IcloudKind::new(auth)), Arc::new(crate::folder::FolderKind)])
    }

    /// Which are on, from the plugins' switches: a built-in kind unless
    /// its plugin's is `false`. Whether that changed anything.
    pub fn refresh(&self, switches: &HashMap<String, bool>) -> bool {
        let on: HashSet<String> =
            self.all.iter().filter(|k| switches.get(k.plugin()) != Some(&false)).map(|k| k.kind().to_owned()).collect();
        let mut current = self.on.write().unwrap_or_else(|e| e.into_inner());
        if *current == on {
            return false;
        }
        *current = on;
        true
    }

    /// The kind of that name, on or not.
    pub fn of(&self, kind: &str) -> Option<Arc<dyn Kind>> {
        self.all.iter().find(|k| k.kind() == kind).cloned()
    }

    /// An entry's kind, on or not.
    pub fn of_entry(&self, entry: &Value) -> Option<Arc<dyn Kind>> {
        self.of(entry["kind"].as_str()?)
    }

    pub fn on(&self, kind: &str) -> bool {
        self.on.read().unwrap_or_else(|e| e.into_inner()).contains(kind)
    }

    pub fn all(&self) -> &[Arc<dyn Kind>] {
        &self.all
    }

    /// An entry's source id; "none" for one of no kind known.
    pub fn id_of(&self, entry: &Value) -> String {
        self.of_entry(entry).map_or_else(|| "none".to_owned(), |k| k.id_of(entry))
    }

    /// Every kind, with its plugin and whether it is on: the `kinds` event.
    pub fn report(&self) -> Value {
        let kinds: Vec<Value> = self
            .all
            .iter()
            .map(|k| {
                json!({"kind": k.kind(), "plugin": k.plugin(), "on": self.on(k.kind()),
                       "needs_sign_in": k.needs_sign_in(), "accepts": k.accepts()})
            })
            .collect();
        json!({"kinds": kinds})
    }

    /// The source to show: the one the settings name if its kind is on,
    /// else the first added whose kind is on, else none. None named
    /// (`"source": null`) is none. The settings are never rewritten for
    /// this, so a kind turned on again shows what it showed before.
    pub fn shown(&self, settings: &Settings) -> Option<Value> {
        let named = settings.source.as_ref()?;
        let on = |entry: &&Value| entry["kind"].as_str().is_some_and(|k| self.on(k));
        std::iter::once(named).chain(&settings.sources).find(on).cloned()
    }

    /// The source an entry is of; none for none. Whether its kind is on
    /// is the caller's to ask.
    pub fn source_for(&self, entry: Option<&Value>) -> Result<Arc<dyn Source>> {
        let Some(entry) = entry else { return Ok(Arc::new(NoSource)) };
        let kind = self.of_entry(entry).ok_or_else(|| anyhow!("unknown source {:?}", entry["kind"].as_str().unwrap_or("")))?;
        kind.source(entry)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::icloud::kind::IcloudKind;

    fn kinds(accounts: &[&str]) -> Kinds {
        let accounts = accounts.iter().map(|a| a.to_string()).collect();
        Kinds::new(vec![
            Arc::new(IcloudKind::with_accounts(PathBuf::from("/nonexistent/pinhole-auth"), accounts)),
            Arc::new(crate::folder::FolderKind),
        ])
    }

    fn off(plugin: &str) -> HashMap<String, bool> {
        [(plugin.to_owned(), false)].into()
    }

    #[test]
    fn a_kind_is_on_unless_its_plugin_is_turned_off() {
        let k = kinds(&[]);
        assert!(k.on("icloud") && k.on("folder"));
        // Nothing said, or said on: on, and nothing changed.
        assert!(!k.refresh(&HashMap::new()));
        assert!(!k.refresh(&[("pinhole.icloud".to_owned(), true), ("pinhole.series".to_owned(), false)].into()));
        assert!(k.refresh(&off("pinhole.icloud")));
        assert!(!k.on("icloud") && k.on("folder"));
        assert!(!k.refresh(&off("pinhole.icloud")));
        let report = k.report();
        assert_eq!(report["kinds"][0], json!({"kind": "icloud", "plugin": "pinhole.icloud", "on": false,
                                              "needs_sign_in": true, "accepts": crate::icloud::upload::ACCEPTS}));
        assert_eq!(report["kinds"][1]["kind"], "folder");
        assert_eq!(report["kinds"][1]["plugin"], "pinhole.folder");
        assert_eq!(report["kinds"][1]["on"], true);
        // Off a kind still is: its entries are known, and its sources made.
        assert!(k.of("icloud").is_some());
        assert_eq!(k.id_of(&json!({"kind": "icloud", "account": "a@x.com"})), "icloud:a@x.com");
        assert_eq!(k.source_for(Some(&json!({"kind": "icloud", "account": "a@x.com"}))).unwrap().id(), "icloud:a@x.com");
        assert!(k.refresh(&HashMap::new()));
        assert!(k.on("icloud"));
        // No kind of that name.
        assert!(k.of("what").is_none());
        assert_eq!(k.id_of(&json!({"kind": "what"})), "none");
        assert!(k.source_for(Some(&json!({"kind": "what"}))).is_err());
        assert_eq!(k.source_for(None).unwrap().kind(), "none");
    }

    #[test]
    fn the_source_shown_is_the_one_named_while_its_kind_is_on() {
        let k = kinds(&[]);
        let a = json!({"kind": "icloud", "account": "a@x.com"});
        let f = json!({"kind": "folder", "path": "/f"});
        let g = json!({"kind": "folder", "path": "/g"});
        let settings = |source: Option<&Value>| Settings {
            sources: vec![a.clone(), f.clone(), g.clone()],
            source: source.cloned(),
            ..Settings::default()
        };
        assert_eq!(k.shown(&settings(Some(&a))), Some(a.clone()));
        assert_eq!(k.shown(&settings(Some(&g))), Some(g.clone()));
        // None named is none, whatever was added.
        assert_eq!(k.shown(&settings(None)), None);
        // Its kind off: the first added whose kind is on.
        k.refresh(&off("pinhole.icloud"));
        assert_eq!(k.shown(&settings(Some(&a))), Some(f.clone()));
        assert_eq!(k.shown(&settings(Some(&g))), Some(g.clone()));
        // Every kind added off: none.
        k.refresh(&[("pinhole.icloud".to_owned(), false), ("pinhole.folder".to_owned(), false)].into());
        assert_eq!(k.shown(&settings(Some(&a))), None);
        // On again: what was named, as the settings never changed.
        k.refresh(&HashMap::new());
        assert_eq!(k.shown(&settings(Some(&a))), Some(a));
        // Only a folder added, iCloud off: the folder.
        k.refresh(&off("pinhole.icloud"));
        let only = Settings { sources: vec![f.clone()], source: Some(f.clone()), ..Settings::default() };
        assert_eq!(k.shown(&only), Some(f));
    }
}
