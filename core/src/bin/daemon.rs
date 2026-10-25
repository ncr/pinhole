//! pinholed: the background service the window asks for photos' larger
//! renditions. See docs/service.md.

use std::path::PathBuf;

use anyhow::Result;
use clap::Parser;

use pinhole_core::service::{self, Config};
use pinhole_core::storage;

#[derive(Parser)]
struct Cli {
    /// The library directory: library.sqlite and thumbs/.
    #[arg(long)]
    library: Option<PathBuf>,
    /// Where fetched files are kept.
    #[arg(long)]
    cache: Option<PathBuf>,
    #[arg(long)]
    socket: Option<PathBuf>,
    /// The sign-in provider (see auth/PROTOCOL.md).
    #[arg(long)]
    auth: Option<PathBuf>,
    /// Downloads at once.
    #[arg(long, default_value_t = 8)]
    downloads: usize,
}

fn env_dir(var: &str, fallback: &str) -> PathBuf {
    std::env::var_os(var)
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from(std::env::var_os("HOME").unwrap_or_default()).join(fallback))
}

fn main() -> Result<()> {
    let cli = Cli::parse();
    // Everything in one folder (storage.rs): as the window last set it in
    // the settings, else ~/.local/share/pinhole; the library and the cache
    // in it unless given.
    let settings = storage::settings_path();
    let saved = storage::read_settings(&settings);
    let location = saved.location.unwrap_or_else(storage::default_location);
    let cache_given = cli.cache.is_some();
    let source = pinhole_core::sync::source_for(
        saved.source.as_ref(),
        cli.auth.unwrap_or_else(pinhole_core::icloud::auth::default_provider),
    )?;
    service::run(Config {
        library: cli.library.unwrap_or_else(|| storage::library_for(&location, source.as_ref())),
        cache: cli.cache.unwrap_or_else(|| location.join("cache")),
        location,
        socket: cli.socket.unwrap_or_else(|| env_dir("XDG_RUNTIME_DIR", ".cache").join("pinhole.sock")),
        source,
        limit: saved.limit,
        settings,
        // Photos still where the cache was before it moved in with the
        // library come over.
        adopt: (!cache_given).then(storage::old_cache),
        downloads: cli.downloads,
        sync_every: std::env::var("PINHOLE_SYNC_SECS").ok().and_then(|v| v.parse().ok()).unwrap_or(60),
    })
}
