//! pinholed: the background service the window asks for photos' larger
//! renditions. See docs/service.md.

use std::path::PathBuf;

use anyhow::Result;
use clap::Parser;

use pinhole_core::service::{self, Config};

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

/// The provider next to this binary when running from a checkout
/// (core/target/release/), otherwise whatever is on PATH.
fn default_auth() -> PathBuf {
    if let Some(p) = std::env::var_os("PINHOLE_AUTH") {
        return PathBuf::from(p);
    }
    if let Ok(exe) = std::env::current_exe() {
        let checkout = exe.ancestors().nth(4).map(|root| root.join("auth/pyicloud/pinhole-auth"));
        if let Some(p) = checkout.filter(|p| p.exists()) {
            return p;
        }
    }
    PathBuf::from("pinhole-auth")
}

fn main() -> Result<()> {
    let cli = Cli::parse();
    let cap_gb: u64 = std::env::var("PINHOLE_CACHE_GB").ok().and_then(|v| v.parse().ok()).unwrap_or(10);
    service::run(Config {
        library: cli.library.unwrap_or_else(|| env_dir("XDG_DATA_HOME", ".local/share").join("pinhole/library")),
        cache: cli.cache.unwrap_or_else(|| env_dir("XDG_CACHE_HOME", ".cache").join("pinhole")),
        socket: cli.socket.unwrap_or_else(|| env_dir("XDG_RUNTIME_DIR", ".cache").join("pinhole.sock")),
        auth: cli.auth.unwrap_or_else(default_auth),
        cache_cap: cap_gb << 30,
        downloads: cli.downloads,
    })
}
