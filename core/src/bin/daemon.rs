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
    // The cache's folder and limit: as given, else as the window last set
    // them (the settings), else ~/.cache/pinhole and 10 GB.
    let settings = storage::settings_path();
    let (saved_cache, saved_cap) = storage::read_settings(&settings);
    let cap = std::env::var("PINHOLE_CACHE_GB").ok().and_then(|v| v.parse::<u64>().ok()).map(|gb| gb << 30);
    service::run(Config {
        library: cli.library.unwrap_or_else(|| env_dir("XDG_DATA_HOME", ".local/share").join("pinhole/library")),
        cache: cli.cache.or(saved_cache).unwrap_or_else(|| env_dir("XDG_CACHE_HOME", ".cache").join("pinhole")),
        socket: cli.socket.unwrap_or_else(|| env_dir("XDG_RUNTIME_DIR", ".cache").join("pinhole.sock")),
        auth: cli.auth.unwrap_or_else(pinhole_core::auth::default_provider),
        cache_cap: cap.or(saved_cap).unwrap_or(storage::DEFAULT_CAP),
        settings,
        downloads: cli.downloads,
        sync_every: std::env::var("PINHOLE_SYNC_SECS").ok().and_then(|v| v.parse().ok()).unwrap_or(60),
    })
}
