# Pinhole

A fast, on-demand iCloud Photos browser for Omarchy. Working name.

Pinhole keeps an index of the whole library on disk (metadata and small
thumbnails) and fetches everything else from iCloud only when it is looked at,
dragged or exported, the way Photos on a Mac does with "Optimize Mac Storage".
It never changes the library.

Status: two spikes. The iCloud API probe is in
[docs/api-findings.md](docs/api-findings.md); the photo grid, at 120 Hz on a
synthetic 100k library, is in [docs/grid-spike.md](docs/grid-spike.md).

## Layout

```text
auth/PROTOCOL.md          the sign-in provider protocol: JSON lines over stdio
auth/pyicloud/pinhole-auth  the first provider, on the pyicloud module from icloudpd
core/                     the Rust core: session, CloudKit client, index
core/src/bin/probe.rs     pinhole-probe, which measures the API
core/src/bin/sync.rs      pinhole-sync, which builds the local library
core/src/bin/daemon.rs    pinholed, the background service: larger renditions on demand
core/src/bin/fake.rs      pinhole-fake, a synthetic library for the grid
app/                      the window: Qt Quick, with the grid in C++
```

## Trying the grid

On your own library (after adopting a session with `import_icloudpd`, below):

```bash
core/target/release/pinhole-sync all   # index (~5 min) then thumbnails (~6 min) into ~/.local/share/pinhole/library
app/bin/build
app/build/pinhole            # S toggles the stats, +/- zoom, Ctrl+wheel zooms, double-click opens
app/build/pinhole --bench    # scripted run, prints frame timing per phase
```

`pinhole-sync` only fetches what is missing, so running it again is quick.
After that, `pinholed` keeps the library in step with iCloud on its own:
new, deleted and edited photos appear within a minute, without a restart.

When cells grow past what the 480 px thumbnails can fill, the window asks
`pinholed` for the 2048 px rendition. The window starts the service if it is
not running; it keeps a size-capped cache in `~/.cache/pinhole` and logs to
`~/.local/state/pinhole/pinholed.log`. Its protocol is in
[docs/service.md](docs/service.md).

Double-click a photo, or press Space or Enter over it, to open it full
window: the thumbnail at once, the medium rendition in about a second, then
the original (HEIC through libheif). Left and Right step through, Escape or
Space closes, Space plays and pauses a video, L plays a Live Photo's motion.
`PINHOLE_SELFTEST=viewer app/build/pinhole` opens one of each kind and
reports how long every stage took.

Click selects (Ctrl-click adds, Shift-click a range, Ctrl+A all). Drag
photos out to any app; Ctrl+C copies them; Ctrl+E exports the originals and
Ctrl+Shift+E a compatible form (HEIC as JPEG, keeping the date, place and
camera details) to a folder chosen in the desktop's own picker. Originals
come from iCloud as they are needed.

Without a real library the window opens the synthetic one:

```bash
core/target/release/pinhole-fake --count 100000   # ~1 min, 6 GB in ~/.local/share/pinhole/fake
app/build/pinhole --library ~/.local/share/pinhole/fake
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
