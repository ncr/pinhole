//! The built-in kinds together, with the Apple IDs given: which are on as
//! their plugins' switches say, the source shown, and the settings resolved
//! through them exactly as before there were kinds. These were the core's
//! own tests until the kinds moved into their plugins' crates.

use std::collections::HashMap;
use std::path::PathBuf;
use std::sync::Arc;

use pinhole_core::kinds::Kinds;
use pinhole_core::storage::{read_settings, remove_source, resolve, show_source, source_entry, Settings};
use pinhole_folder::FolderKind;
use pinhole_icloud::kind::IcloudKind;
use serde_json::{json, Value};

fn temp(name: &str) -> PathBuf {
    let dir = std::env::temp_dir().join(format!("pinhole-builtin-kinds-{name}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).unwrap();
    dir
}

/// The built-in kinds, with these Apple IDs taken as signed in to.
fn kinds(accounts: &[&str]) -> Kinds {
    let accounts = accounts.iter().map(|a| a.to_string()).collect();
    Kinds::new(vec![
        Arc::new(IcloudKind::with_accounts(PathBuf::from("/nonexistent/pinhole-auth"), accounts)),
        Arc::new(FolderKind),
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
                                          "needs_sign_in": true, "accepts": pinhole_icloud::upload::ACCEPTS}));
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
#[test]
fn without_a_list_the_sources_are_the_apple_ids_then_the_folder_shown() {
    let two = kinds(&["a@x.com", "b@x.com"]);
    let folder = json!({"kind": "folder", "path": "/photos"});
    let s = resolve(&json!({"source": folder}), &two);
    assert_eq!(s.sources, vec![
        json!({"kind": "icloud", "account": "a@x.com"}),
        json!({"kind": "icloud", "account": "b@x.com"}),
        folder.clone(),
    ]);
    assert_eq!(s.source, Some(folder));
    // Nothing said: the first of them; nothing at all: none.
    assert_eq!(resolve(&json!({}), &two).source, Some(json!({"kind": "icloud", "account": "a@x.com"})));
    assert_eq!(resolve(&json!({}), &kinds(&[])), Settings::default());
    // `null` is none, though there are sources.
    assert_eq!(resolve(&json!({"source": null}), &two).source, None);
    // An iCloud entry without an Apple ID is the only one signed in to.
    let one = resolve(&json!({"source": {"kind": "icloud"}}), &kinds(&["a@x.com"]));
    assert_eq!(one.source, Some(json!({"kind": "icloud", "account": "a@x.com"})));
}

#[test]
fn a_list_is_kept_in_order_without_repeats() {
    let k = kinds(&["z@x.com"]);
    let s = resolve(
        &json!({"sources": [
            {"kind": "folder", "path": "/b"}, {"kind": "icloud", "account": "a@x.com"},
            {"kind": "folder", "path": "/b"}, {"kind": "what"},
        ]}),
        &k,
    );
    assert_eq!(s.sources, vec![json!({"kind": "folder", "path": "/b"}), json!({"kind": "icloud", "account": "a@x.com"})]);
    assert_eq!(s.source, Some(json!({"kind": "folder", "path": "/b"})));
    assert_eq!(k.id_of(&s.sources[1]), "icloud:a@x.com");
    assert_eq!(source_entry(&s, &k, "folder:/b"), Some(json!({"kind": "folder", "path": "/b"})));
}

/// `resolve` as it was before source kinds (7d06da2), word for word
/// but for its name: what every input must still resolve to.
fn resolve_before_kinds(v: &Value, accounts: &[String]) -> Settings {
    let source_id = |entry: &Value| -> String {
        match entry["kind"].as_str() {
            Some("icloud") => format!("icloud:{}", entry["account"].as_str().unwrap_or("")),
            Some("folder") => format!("folder:{}", entry["path"].as_str().unwrap_or("")),
            _ => "none".into(),
        }
    };
    let normal = |entry: &Value| -> Option<Value> {
        match entry["kind"].as_str()? {
            "icloud" => {
                let account = entry["account"].as_str().map(str::to_owned).or_else(|| (accounts.len() == 1).then(|| accounts[0].clone()))?;
                Some(json!({"kind": "icloud", "account": account}))
            }
            "folder" => Some(json!({"kind": "folder", "path": entry["path"].as_str()?})),
            _ => None,
        }
    };
    let shown = match v.get("source") {
        None => None,
        Some(Value::Null) => Some(None),
        Some(entry) => Some(normal(entry)),
    };
    let mut sources: Vec<Value> = match v["sources"].as_array() {
        Some(list) => list.iter().filter_map(normal).collect(),
        None => {
            let mut list: Vec<Value> = accounts.iter().map(|a| json!({"kind": "icloud", "account": a})).collect();
            if let Some(Some(entry)) = &shown {
                if entry["kind"] == "folder" {
                    list.push(entry.clone());
                }
            }
            list
        }
    };
    let mut seen = std::collections::HashSet::new();
    sources.retain(|e| seen.insert(source_id(e)));
    let source = match shown {
        Some(entry) => entry,
        None => sources.first().cloned(),
    };
    Settings {
        location: v["location"].as_str().map(PathBuf::from),
        limit: v["limit_bytes"].as_u64(),
        sources,
        source,
    }
}

#[test]
fn every_setting_resolves_as_it_did_before_source_kinds_whichever_kinds_are_on() {
    let jacek = json!({"kind": "icloud", "account": "jacek@example.com"});
    let a = json!({"kind": "icloud", "account": "a@x.com"});
    let b = json!({"kind": "icloud", "account": "b@x.com"});
    let f = json!({"kind": "folder", "path": "/photos"});
    let g = json!({"kind": "folder", "path": "/home/me/Pictures/iCloud"});
    let inputs = [
        // Jacek's: no settings.json at all.
        json!({}),
        json!({"location": "/mnt/photos", "limit_bytes": 21474836480u64}),
        json!({"location": 5, "limit_bytes": "big"}),
        json!({"source": null}),
        json!({"source": f}),
        json!({"source": a}),
        json!({"source": b}),
        json!({"source": jacek}),
        json!({"source": {"kind": "icloud"}}),
        json!({"source": {"kind": "icloud", "account": null}}),
        json!({"source": {"kind": "folder"}}),
        json!({"source": {"kind": "what", "path": "/x"}}),
        json!({"source": {"path": "/x"}}),
        json!({"source": "folder"}),
        json!({"sources": []}),
        json!({"sources": [], "source": f}),
        json!({"sources": [], "source": null}),
        json!({"sources": [f, a, f, {"kind": "what"}, {"kind": "folder"}, {"kind": "icloud"}, g]}),
        json!({"sources": [g, f], "source": f}),
        json!({"sources": [g, f], "source": a}),
        json!({"sources": [a, b, jacek], "source": {"kind": "icloud"}}),
        json!({"sources": [{"kind": "icloud"}]}),
        json!({"sources": "not a list", "source": g}),
        json!({"sources": {"0": f}}),
        json!({"sources": [f], "source": null, "location": "/elsewhere", "moving": {"from": "/a", "to": "/b"}}),
    ];
    let account_sets: [&[&str]; 4] = [&[], &["jacek@example.com"], &["a@x.com", "b@x.com"], &["b@x.com"]];
    let switch_sets: [&[(&str, bool)]; 4] =
        [&[], &[("pinhole.icloud", false)], &[("pinhole.folder", false)], &[("pinhole.icloud", false), ("pinhole.folder", false)]];
    for accounts in account_sets {
        let owned: Vec<String> = accounts.iter().map(|a| a.to_string()).collect();
        let k = kinds(accounts);
        for switches in switch_sets {
            k.refresh(&switches.iter().map(|(id, on)| (id.to_string(), *on)).collect());
            for input in &inputs {
                assert_eq!(resolve(input, &k), resolve_before_kinds(input, &owned), "{input} with {accounts:?}, {switches:?}");
            }
        }
    }
    // Jacek's own: no settings, one Apple ID signed in to by its session.
    let s = resolve(&json!({}), &kinds(&["jacek@example.com"]));
    assert_eq!(s.sources, vec![jacek.clone()]);
    assert_eq!(s.source, Some(jacek));
    assert_eq!((s.location, s.limit), (None, None));
}

#[test]
fn sources_are_added_shown_and_removed() {
    let dir = temp("sources");
    let path = dir.join("settings.json");
    let k = kinds(&[]);
    std::fs::write(&path, r#"{"location": "/keep", "sources": [{"kind": "folder", "path": "/a"}]}"#).unwrap();
    let b = json!({"kind": "folder", "path": "/b"});
    show_source(&path, &k, Some(&b)).unwrap();
    let s = read_settings(&path, &k);
    assert_eq!(s.sources, vec![json!({"kind": "folder", "path": "/a"}), b.clone()]);
    assert_eq!(s.source, Some(b.clone()));
    assert_eq!(s.location, Some(PathBuf::from("/keep")));
    // Shown again: not added twice.
    show_source(&path, &k, Some(&b)).unwrap();
    assert_eq!(read_settings(&path, &k).sources.len(), 2);
    // The one shown goes: the first left is shown; then none.
    assert_eq!(remove_source(&path, &k, "folder:/b").unwrap(), Some(json!({"kind": "folder", "path": "/a"})));
    assert_eq!(remove_source(&path, &k, "folder:/a").unwrap(), None);
    let s = read_settings(&path, &k);
    assert!(s.sources.is_empty());
    assert_eq!(s.source, None);
}
