//! pinhole-sync: bring the local library up to date with its source (the
//! one the settings name, iCloud unless they name another). The index
//! first (every asset's metadata), then the thumbnails the grid shows. Both
//! steps pick up where they left off and only fetch what is missing.

use std::path::PathBuf;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::time::Instant;

use anyhow::{bail, Result};
use clap::{Parser, Subcommand};

use pinhole_core::icloud::auth::ProcessProvider;
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
    let settings = storage::read_settings(&storage::settings_path());
    let source = sync::source_for(settings.source.as_ref(), auth.clone())?;
    if let Cmd::SignIn { from_icloudpd } = &cli.command {
        if !source.needs_sign_in() {
            bail!("{} needs no signing in", source.name());
        }
        return sign_in(auth, from_icloudpd.as_deref());
    }
    // In the folder the window keeps everything in (storage.rs), the
    // source's own library.
    let library = cli.library.unwrap_or_else(|| storage::library_for(&storage::location(), source.as_ref()));
    std::fs::create_dir_all(&library)?;
    let mut conn = db::open(&library.join("library.sqlite"))?;
    let source = source.as_ref();

    let (index, thumbs) = match cli.command {
        Cmd::Index => (true, None),
        Cmd::Thumbs { parallel } => (false, Some(parallel)),
        Cmd::All => (true, Some(64)),
        Cmd::SignIn { .. } => unreachable!("handled above"),
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
        })?;
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
        })?;
        println!("thumbs: {} of {} made in {:.0}s ({:.0}/s, {:.2} GB fetched); {} failed",
            r.made, r.wanted, r.seconds, r.made as f64 / r.seconds.max(1e-3), r.bytes as f64 / 1e9, r.failed);
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

fn sign_in(auth: PathBuf, from_icloudpd: Option<&str>) -> Result<()> {
    let mut provider = ProcessProvider::spawn(auth, None)?;
    if let Some(apple_id) = from_icloudpd {
        provider.call("import_icloudpd", serde_json::json!({"apple_id": apple_id}))?;
        let status = provider.call("status", serde_json::Value::Null)?;
        println!("{}", if status["state"] == "signed_in" { "Signed in, with icloudpd's session." } else { "icloudpd's session is no longer valid; sign in without --from-icloudpd." });
        return Ok(());
    }
    let apple_id = ask("Apple ID: ", false)?;
    let password = ask("Password: ", true)?;
    let state = provider.call("sign_in", serde_json::json!({"apple_id": apple_id, "password": password}))?;
    if state["state"] == "needs_code" {
        let code = ask("The six-digit code Apple sent to your devices: ", false)?;
        provider.call("submit_code", serde_json::json!({"code": code.trim()}))?;
    }
    println!("Signed in as {apple_id}. Next: pinhole-sync all");
    Ok(())
}
