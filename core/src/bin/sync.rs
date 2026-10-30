//! pinhole-sync: bring the local library up to date with its source (the
//! one the settings name, iCloud unless they name another). The index
//! first (every asset's metadata), then the thumbnails the grid shows. Both
//! steps pick up where they left off and only fetch what is missing.

use std::path::PathBuf;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::time::Instant;

use anyhow::{bail, Result};
use clap::{Parser, Subcommand};

use pinhole_core::icloud::{self, auth::ProcessProvider};
use pinhole_core::{db, storage, sync};

#[derive(Parser)]
struct Cli {
    /// The sign-in provider to run, for iCloud (see auth/PROTOCOL.md).
    #[arg(long)]
    auth: Option<PathBuf>,
    /// The library directory: library.sqlite and thumbs/.
    #[arg(long)]
    library: Option<PathBuf>,
    #[command(subcommand)]
    command: Cmd,
}

#[derive(Subcommand)]
enum Cmd {
    /// Read every asset's metadata into the index.
    Index,
    /// Make the thumbnails the index does not have yet.
    Thumbs {
        #[arg(long, default_value_t = 64)]
        parallel: usize,
    },
    /// Index, then thumbnails.
    All,
    /// Sign in to iCloud: Apple ID, password and the two-factor code, asked
    /// for here. The provider keeps the session; the password is not stored.
    SignIn {
        /// Take over the session icloudpd already has for this Apple ID
        /// instead (copied from ~/.config/icloudpd).
        #[arg(long, value_name = "APPLE_ID")]
        from_icloudpd: Option<String>,
    },
    /// Add photos and videos (files, or folders of them) to the source:
    /// copied into a folder, uploaded to iCloud. The files stay as they are.
    Add {
        #[arg(required = true)]
        files: Vec<PathBuf>,
    },
    /// Apply what changed in the source since the last listing or sync,
    /// then make thumbnails for anything new.
    Changes {
        /// Only report what would change.
        #[arg(long)]
        dry_run: bool,
    },
}

fn main() -> Result<()> {
    let cli = Cli::parse();
    let auth = cli.auth.unwrap_or_else(pinhole_core::icloud::auth::default_provider);
    // Signing in is iCloud's whatever the source is now: it makes the
    // Apple ID the source, so a folder is no reason to refuse.
    if let Cmd::SignIn { from_icloudpd } = &cli.command {
        return sign_in(auth, from_icloudpd.as_deref());
    }
    let settings = storage::read_settings(&storage::settings_path());
    let source = sync::source_for(settings.source.as_ref(), auth.clone())?;
    if source.kind() == "none" {
        bail!("nothing to show photos from yet: pinhole-sync sign-in, or add a folder in the window");
    }
    if let Cmd::Add { files } = &cli.command {
        return add(source.as_ref(), files);
    }
    // In the folder the window keeps everything in (storage.rs), the
    // source's own library.
    let library = cli.library.unwrap_or_else(|| storage::library_for(&storage::location(), source.as_ref()));
    std::fs::create_dir_all(&library)?;
    // One writer at a time: pinholed's rounds wait while this writes.
    let _writer = match sync::Writer::try_take(&library)? {
        Some(writer) => writer,
        None => {
            eprintln!("pinholed is writing this library now; waiting for it to finish…");
            sync::Writer::take(&library)?
        }
    };
    let mut conn = db::open(&library.join("library.sqlite"))?;
    let source = source.as_ref();
    sync::claim(source, &mut conn, &library)?;

    let (index, thumbs) = match cli.command {
        Cmd::Index => (true, None),
        Cmd::Thumbs { parallel } => (false, Some(parallel)),
        Cmd::All => (true, Some(64)),
        Cmd::SignIn { .. } | Cmd::Add { .. } => unreachable!("handled above"),
        Cmd::Changes { dry_run } => {
            let report = sync::follow(source, &mut conn, dry_run)?;
            let r = &report.changes;
            let name = |id: &String| -> String {
                conn.query_row("SELECT filename FROM assets WHERE id = ?1", [id], |r| r.get::<_, Option<String>>(0))
                    .ok()
                    .flatten()
                    .unwrap_or_else(|| id.clone())
            };
            println!("{}{}: {} new, {} changed, {} removed",
                if dry_run { "dry run: " } else { "" }, report.summary, r.added.len(), r.updated.len(), r.removed.len());
            if !r.reasons.is_empty() {
                println!("  changed, by what: {:?}", r.reasons);
            }
            for (label, ids) in [("new", &r.added), ("changed", &r.updated), ("removed", &r.removed)] {
                let shown: Vec<String> = ids.iter().take(8).map(name).collect();
                if !shown.is_empty() {
                    println!("  {label}: {}{}", shown.join(", "), if ids.len() > 8 { ", …" } else { "" });
                }
            }
            if dry_run || r.added.is_empty() {
                return Ok(());
            }
            (false, Some(16))
        }
    };

    if index {
        let shown = AtomicUsize::new(0);
        let r = sync::list(source, &mut conn, &|done, total| {
            // A line every 5,000 assets.
            let bucket = (done / 5000) as usize;
            if shown.swap(bucket, Ordering::Relaxed) != bucket {
                eprintln!("index {done:>7}/{total}");
            }
        }, &|| false)?;
        let total: i64 = conn.query_row("SELECT count(*) FROM assets", [], |r| r.get(0))?;
        println!("index: {total} assets from {} in {:.0}s; {} new, {} removed; {}",
            source.name(), r.seconds, r.changes.added.len(), r.changes.removed.len(), r.summary);
    }

    if let Some(parallel) = thumbs {
        let started = Instant::now();
        let shown = AtomicUsize::new(0);
        let r = sync::make_thumbs(source, &mut conn, &library, parallel, &|done, total| {
            let bucket = done / 5000;
            if shown.swap(bucket, Ordering::Relaxed) != bucket {
                eprintln!("thumbs {done:>7}/{total}  {:.0}/s", done as f64 / started.elapsed().as_secs_f64());
            }
        }, &|| false)?;
        println!("thumbs: {} of {} made in {:.0}s ({:.0}/s, {:.2} GB fetched); {} failed",
            r.made, r.wanted, r.seconds, r.made as f64 / r.seconds.max(1e-3), r.bytes as f64 / 1e9, r.failed);
        let covers = sync::make_covers(source, &mut conn, &library, &|| false)?;
        println!("covers: {covers} dull video thumbnails replaced");
    }

    let (total, with_thumbs): (i64, i64) = conn.query_row(
        "SELECT count(*), count(thumb_path) FROM assets", [], |r| Ok((r.get(0)?, r.get(1)?)))?;
    println!("library {}: {total} assets, {with_thumbs} with thumbnails", library.display());
    Ok(())
}

fn ask(prompt: &str, hidden: bool) -> Result<String> {
    use std::io::Write;
    eprint!("{prompt}");
    std::io::stderr().flush()?;
    // Not echoed: the terminal's echo is off while the password is typed.
    let restore = hidden && std::process::Command::new("stty").arg("-echo").status().is_ok_and(|s| s.success());
    let mut line = String::new();
    let read = std::io::stdin().read_line(&mut line);
    if restore {
        let _ = std::process::Command::new("stty").arg("echo").status();
        eprintln!();
    }
    read?;
    Ok(line.trim_end_matches(['\r', '\n']).to_owned())
}

/// Sign in to an Apple ID, its session kept apart from any other's, and
/// show its photos from now on (the settings' source; a running service
/// follows).
fn sign_in(auth: PathBuf, from_icloudpd: Option<&str>) -> Result<()> {
    let apple_id = match from_icloudpd {
        Some(apple_id) => apple_id.to_owned(),
        None => ask("Apple ID: ", false)?,
    };
    let mut provider = ProcessProvider::spawn(auth, Some(icloud::auth::state_dir(&apple_id)))?;
    if from_icloudpd.is_some() {
        provider.call("import_icloudpd", serde_json::json!({"apple_id": apple_id}))?;
        let status = provider.call("status", serde_json::Value::Null)?;
        if status["state"] != "signed_in" {
            bail!("icloudpd's session is no longer valid; sign in without --from-icloudpd");
        }
    } else {
        let password = ask("Password: ", true)?;
        let state = provider.call("sign_in", serde_json::json!({"apple_id": apple_id, "password": password}))?;
        if state["state"] == "needs_code" {
            let code = ask("The six-digit code Apple sent to your devices: ", false)?;
            provider.call("submit_code", serde_json::json!({"code": code.trim()}))?;
        }
    }
    storage::show_source(&storage::settings_path(), Some(&serde_json::json!({"kind": "icloud", "account": apple_id})))?;
    println!("Signed in as {apple_id}; Pinhole shows its photos. Next: pinhole-sync all");
    Ok(())
}

/// Each asset in turn, saying how it went; an error at the end if any
/// failed. The library hears of them at its next round (`changes`).
fn add(source: &dyn pinhole_core::source::Source, files: &[PathBuf]) -> Result<()> {
    let (additions, other) = pinhole_core::add::gather(files);
    for path in &other {
        println!("{}: not a photo or video", path.display());
    }
    let mut failed = other.len();
    for addition in &additions {
        let name = addition.file.display();
        let shown = AtomicUsize::new(0);
        let result = source.add(addition, &|done, total| {
            // A line every tenth.
            let tenth = (done * 10 / total.max(1)) as usize;
            if shown.swap(tenth, Ordering::Relaxed) != tenth && tenth < 10 {
                eprintln!("{name}: {}%", tenth * 10);
            }
        });
        match result {
            Ok(added) if added.duplicate => println!("{name}: {} has it already", source.name()),
            Ok(added) => {
                println!("{name}: added{}", added.id.map(|id| format!(" as {id}")).unwrap_or_default());
                if let Some(note) = added.note {
                    println!("  {note}");
                }
            }
            Err(e) => {
                failed += 1;
                println!("{name}: {e:#}");
            }
        }
    }
    if failed > 0 {
        bail!("{failed} not added");
    }
    Ok(())
}
