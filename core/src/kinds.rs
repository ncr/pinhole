//! Source kinds: what pinholed knows of each kind of source apart from the
//! `Source` it makes (docs/core-and-plugins.md, "Source kinds"). A kind
//! says how its settings entries look and what their source id is, makes
//! the source, describes it for the window, adds one, and signs in and out
//! where that means anything. The service names no kind itself.
//!
//! Each kind follows the switch of the plugin that owns it in
//! `plugins.json` (plugins.rs). The built-in kinds are compiled into
//! pinholed, each tied to its plugin's id there (core/daemon: pinhole.icloud,
//! pinhole.folder) and never claimed by a manifest. A kind that is off keeps
//! its entries in the settings and its libraries on disk; it is only not
//! listed, not shown and not synced, until it is on again.
//!
//! A plugin may also bring a kind of its own, run out of process by its
//! provider (process.rs, docs/source-providers.md). Those are found in the
//! plugins' folders at each `refresh`, and come and go with them.

use std::collections::{HashMap, HashSet};
use std::sync::{Arc, Mutex, RwLock};

use anyhow::{anyhow, Result};
use serde_json::{json, Value};

use crate::plugins::Places;
use crate::process::{Limits, ProcessKind};
use crate::source::{NoSource, Source};
use crate::storage::Settings;

pub trait Kind: Send + Sync {
    /// As settings.json names it: "folder", or pinhole.icloud's own.
    fn kind(&self) -> &str;

    /// The plugin whose switch it follows: "pinhole.folder".
    fn plugin(&self) -> &str;

    /// An entry of this kind in its usual form, or None for one that
    /// names nothing.
    fn normal(&self, entry: &Value) -> Option<Value>;

    /// The entry's source id, as its source's `id()` says it: the kind,
    /// a colon and what tells its sources apart ("folder:<path>").
    fn id_of(&self, entry: &Value) -> String;

    /// Entries known without the settings naming them (pinhole.icloud's:
    /// the accounts signed in to here).
    fn implicit(&self) -> Vec<Value> {
        Vec::new()
    }

    /// Whether, with no list in the settings, its sources are only its
    /// `implicit` ones: one shown that is not among them is not added
    /// (pinhole.icloud's account not signed in to). A kind with none (a
    /// folder) has the one shown added after them.
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

    /// Taken out of the sources: what it kept apart goes (pinhole.icloud's
    /// session). Its library is the service's to delete.
    fn removed(&self, _entry: &Value) {}

    /// Its plugin's switch, as `Kinds::refresh` finds it: a provider's
    /// processes stop when it is turned off (process.rs).
    fn set_on(&self, _on: bool) {}
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
    /// Compiled into pinholed.
    builtin: Vec<Arc<dyn Kind>>,
    /// Where to look for plugins' providers, and what they may do; none,
    /// and there are only the built-in kinds.
    places: Option<(Places, Limits)>,
    /// The kinds of providers the plugins there declare, as last looked.
    providers: RwLock<Vec<Arc<ProcessKind>>>,
    /// Why any declared were refused, as last said in the log.
    refused: Mutex<Vec<String>>,
    on: RwLock<HashSet<String>>,
}

impl Kinds {
    /// These kinds, in this order (the order sources are implied in), all
    /// on until `refresh` says otherwise.
    pub fn new(all: Vec<Arc<dyn Kind>>) -> Kinds {
        let on = all.iter().map(|k| k.kind().to_owned()).collect();
        Kinds { builtin: all, places: None, providers: RwLock::default(), refused: Mutex::default(), on: RwLock::new(on) }
    }

    /// And the kinds of the providers the plugins in `places` declare,
    /// found at each `refresh` (docs/source-providers.md).
    pub fn with_providers(mut self, places: Places, limits: Limits) -> Kinds {
        self.places = Some((places, limits));
        self
    }

    /// Which are on, from the plugins' switches: a built-in kind unless
    /// its plugin's is `false` (only when it is `true` while a plugin of
    /// your own has that plugin's id: your copy of it, off in the window
    /// until turned on); a provider's of a built-in plugin the same,
    /// and of a plugin of your own only when its switch is `true`, as the
    /// window has them. The providers are looked for again first: those of
    /// a plugin gone, or whose manifest changed, are stopped. Whether that
    /// changed anything.
    pub fn refresh(&self, switches: &HashMap<String, bool>) -> bool {
        let mut kinds_changed = false;
        if let Some((places, limits)) = &self.places {
            let compiled: Vec<&str> = self.builtin.iter().map(|k| k.kind()).collect();
            let (found, refused) = crate::plugins::provided(places, &compiled);
            {
                let mut said = self.refused.lock().unwrap_or_else(|e| e.into_inner());
                for why in refused.iter().filter(|why| !said.contains(why)) {
                    eprintln!("pinholed: a source refused: {why}");
                }
                *said = refused;
            }
            let mut providers = self.providers.write().unwrap_or_else(|e| e.into_inner());
            let next: Vec<Arc<ProcessKind>> = found
                .into_iter()
                .map(|p| match providers.iter().find(|k| *k.provided() == p) {
                    Some(same) => same.clone(),
                    None => Arc::new(ProcessKind::new(p, &places.state, *limits)),
                })
                .collect();
            for old in providers.iter().filter(|old| !next.iter().any(|k| Arc::ptr_eq(k, old))) {
                old.set_on(false);
                kinds_changed = true;
            }
            kinds_changed |= next.iter().any(|new| !providers.iter().any(|k| Arc::ptr_eq(k, new)));
            *providers = next;
        }
        let providers = self.providers.read().unwrap_or_else(|e| e.into_inner());
        let copies = self.places.as_ref().map(|(places, _)| crate::plugins::personal_ids(places)).unwrap_or_default();
        let mut on: HashSet<String> = self
            .builtin
            .iter()
            .filter(|k| {
                let switch = switches.get(k.plugin()).copied();
                if copies.contains(k.plugin()) { switch == Some(true) } else { switch != Some(false) }
            })
            .map(|k| k.kind().to_owned())
            .collect();
        for p in providers.iter() {
            let switch = switches.get(&p.provided().plugin).copied();
            let turned_on = if p.provided().builtin { switch != Some(false) } else { switch == Some(true) };
            p.set_on(turned_on);
            if turned_on {
                on.insert(p.kind().to_owned());
            }
        }
        let mut current = self.on.write().unwrap_or_else(|e| e.into_inner());
        if *current == on {
            return kinds_changed;
        }
        *current = on;
        true
    }

    /// Where the plugins are, if their providers are looked for.
    pub fn places(&self) -> Option<&Places> {
        self.places.as_ref().map(|(places, _)| places)
    }

    /// The kind of that name, on or not.
    pub fn of(&self, kind: &str) -> Option<Arc<dyn Kind>> {
        self.all().into_iter().find(|k| k.kind() == kind)
    }

    /// Whether a kind is a provider's (process.rs), not compiled in.
    pub fn provided(&self, kind: &str) -> bool {
        self.providers.read().unwrap_or_else(|e| e.into_inner()).iter().any(|k| k.kind() == kind)
    }

    /// An entry's kind, on or not.
    pub fn of_entry(&self, entry: &Value) -> Option<Arc<dyn Kind>> {
        self.of(entry["kind"].as_str()?)
    }

    pub fn on(&self, kind: &str) -> bool {
        self.on.read().unwrap_or_else(|e| e.into_inner()).contains(kind)
    }

    /// The built-in kinds, then the providers'.
    pub fn all(&self) -> Vec<Arc<dyn Kind>> {
        let providers = self.providers.read().unwrap_or_else(|e| e.into_inner());
        self.builtin.iter().cloned().chain(providers.iter().map(|k| k.clone() as Arc<dyn Kind>)).collect()
    }

    /// An entry's source id; "none" for one of no kind known, but for one
    /// a provider gave (`kept`), whose plugin is not here now: its id as
    /// any provider's is.
    pub fn id_of(&self, entry: &Value) -> String {
        match self.of_entry(entry) {
            Some(kind) => kind.id_of(entry),
            None if kept(entry) => format!("{}:{}", entry["kind"].as_str().unwrap_or(""), entry["key"].as_str().unwrap_or("")),
            None => "none".to_owned(),
        }
    }

    /// Every kind, with its plugin and whether it is on: the `kinds` event.
    pub fn report(&self) -> Value {
        let kinds: Vec<Value> = self
            .all()
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

/// An entry of a provider's kind, as the settings keep it whether or not
/// its plugin is there now: a kind and a key. Its plugin gone for a while
/// (moved, being reinstalled), it stays among the sources, as one turned
/// off does, and comes back with it.
pub fn kept(entry: &Value) -> bool {
    entry["kind"].as_str().is_some_and(|k| !k.is_empty()) && entry["key"].as_str().is_some_and(|k| !k.is_empty())
}
