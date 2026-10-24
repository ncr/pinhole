//! pinhole-sync: bring the local library up to date with iCloud. The index
//! first (every asset's metadata), then the thumbnails the grid shows. Both
//! steps pick up where they left off and only fetch what is missing.

use std::path::PathBuf;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::time::Instant;

use anyhow::Result;
use clap::{Parser, Subcommand};

use pinhole_core::auth::ProcessProvider;
use pinhole_core::cloudkit::PhotosClient;
use pinhole_core::{db, sync};

#[derive(Parser)]
struct Cli {
    /// The sign-in provider to run (see auth/PROTOCOL.md).
    #[arg(long, default_value = "auth/pyicloud/pinhole-auth")]
    auth: PathBuf,
    /// The library directory: library.sqlite and thumbs/.
    #[arg(long)]
    library: Option<PathBuf>,
    #[command(subcommand)]
    command: Cmd,
}

#[derive(Subcommand)]
enum Cmd {
    /// Read every asset's metadata into the index.
    Index {
        #[arg(long, default_value_t = 4)]
        parallel: usize,
    },
    /// Download the thumbnails the index does not have yet.
    Thumbs {
        #[arg(long, default_value_t = 64)]
        parallel: usize,
    },
    /// Index, then thumbnails.
    All,
}

fn main() -> Result<()> {
    let cli = Cli::parse();
    let library = cli.library.unwrap_or_else(|| {
        PathBuf::from(std::env::var_os("HOME").unwrap_or_default()).join(".local/share/pinhole/library")
    });
    std::fs::create_dir_all(&library)?;
    let mut conn = db::open(&library.join("library.sqlite"))?;
    let mut client = PhotosClient::new(Box::new(ProcessProvider::spawn(cli.auth, None)?))?;

    let (index, thumbs) = match cli.command {
        Cmd::Index { parallel } => (Some(parallel), None),
        Cmd::Thumbs { parallel } => (None, Some(parallel)),
        Cmd::All => (Some(4), Some(64)),
    };

    if let Some(parallel) = index {
        let shown = AtomicUsize::new(0);
        let r = sync::walk(&mut client, &mut conn, 100, parallel, false, &|done, total| {
            // A line every 5,000 assets.
            let bucket = (done / 5000) as usize;
            if shown.swap(bucket, Ordering::Relaxed) != bucket {
                eprintln!("index {done:>7}/{total}");
            }
        })?;
        println!("index: {} assets in {:.0}s; {} split pairs repaired, {} unpaired, {} removed; iCloud reports {}",
            r.assets + r.repaired as u64, r.seconds, r.repaired, r.unpaired, r.removed, r.reported);
    }

    if let Some(parallel) = thumbs {
        let started = Instant::now();
        let shown = AtomicUsize::new(0);
        let r = sync::download_thumbs(&mut client, &mut conn, &library, parallel, &|done, total| {
            let bucket = done / 5000;
            if shown.swap(bucket, Ordering::Relaxed) != bucket {
                eprintln!("thumbs {done:>7}/{total}  {:.0}/s", done as f64 / started.elapsed().as_secs_f64());
            }
        })?;
        println!("thumbs: {} of {} downloaded in {:.0}s ({:.0}/s, {:.2} GB); {} links refreshed, {} failed",
            r.downloaded, r.wanted, r.seconds, r.downloaded as f64 / r.seconds.max(1e-3), r.bytes as f64 / 1e9,
            r.refreshed, r.failed);
    }

    let (total, with_thumbs): (i64, i64) = conn.query_row(
        "SELECT count(*), count(thumb_path) FROM assets", [], |r| Ok((r.get(0)?, r.get(1)?)))?;
    println!("library {}: {total} assets, {with_thumbs} with thumbnails", library.display());
    Ok(())
}
