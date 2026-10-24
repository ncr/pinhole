# Pinhole

A fast, on-demand iCloud Photos browser for Omarchy. Working name.

Pinhole keeps an index of the whole library on disk (metadata and small
thumbnails) and fetches everything else from iCloud only when it is looked at,
dragged or exported, the way Photos on a Mac does with "Optimize Mac Storage".
It never changes the library.

Status: an API probe. There is no window yet. What it found is in
[docs/api-findings.md](docs/api-findings.md).

## Layout

```text
auth/PROTOCOL.md          the sign-in provider protocol: JSON lines over stdio
auth/pyicloud/pinhole-auth  the first provider, on the pyicloud module from icloudpd
core/                     the Rust core: session, CloudKit client, index
core/src/bin/probe.rs     pinhole-probe, which measures the API
```

The two boundaries that are meant to be swapped:

- **Sign-in.** The core starts a provider process and asks it for a session
  (cookies, service URLs, query params). How the provider signs in is its own
  business. A native provider only has to speak `auth/PROTOCOL.md`.
- **Photo source.** `core/src/source.rs` defines `PhotoSource`. iCloud is one
  implementation; a synthetic library for tests and grid stress runs is next.

## Trying the probe

The provider runs with [uv](https://docs.astral.sh/uv/), which fetches
icloudpd into its cache on first use. To adopt a session icloudpd already
signed in (it is copied, not shared):

```bash
echo '{"id":1,"method":"import_icloudpd","params":{"apple_id":"you@example.com"}}' | auth/pyicloud/pinhole-auth
```

Then, from the repository root:

```bash
cargo build --release --manifest-path core/Cargo.toml
core/target/release/pinhole-probe count
core/target/release/pinhole-probe walk --parallel 4
core/target/release/pinhole-probe stats
core/target/release/pinhole-probe changes
core/target/release/pinhole-probe sample --kind JPEGThumb --n 200
core/target/release/pinhole-probe stream
```

The index lands in `~/.local/share/pinhole/probe.sqlite`; the provider keeps
its session in `~/.local/state/pinhole/auth`, readable only by you.
