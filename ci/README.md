# Reproducible checks

GitHub Actions runs the same commands as a local checkout. Docker is the
only host dependency; no packages, windows, clipboard, library or service
are installed on the host by these checks.

```sh
ci/run rust       # Cargo workspace tests and independent JPEG regressions
ci/run ui         # Qt, query/accessibility/frame contracts, plugins, installer
ci/run visual     # Xvfb rendering and application interactions
ci/run native     # private headless Wayland compositor, startup and scrolling
ci/run package    # pkgbuild/PKGBUILD built with makepkg, then looked into
ci/run all
```

The Docker base is pinned by digest and its Arch packages by archive date
in `Dockerfile`, including the Rust and Cargo packages.
Every job records its exact package versions, compiler, command, duration
and exit status, retaining logs even on failure. Results are written under
`.ci-results/<mode>/` (ignored by Git). CI uploads them as artifacts.
The container never sees the checkout. `ci/run` first makes a snapshot of
it in a temporary folder: a clone of its commit, without the remote, with
its tracked and unignored working files laid over, so local checks include
uncommitted changes and work from a linked worktree too. Git config,
credentials and ignored files (build output, `.tools/`) are never in it.
That snapshot is mounted read-only, and the checks run on a private copy.

The package job builds the recipe the AUR gets, from that source instead
of GitHub's, and fails when the package lacks a program or any runtime file
of a plugin or the bar widget, or when its `update.json` names a Git remote
or the folder it was built in. The package's file list is kept as evidence.

Cargo resolves against the checked-in lockfiles. The dependency download
cache is keyed by the container recipe and those lockfiles. The Rust job
also decodes the JPEG fixtures with SIMD and with scalar decoding.

Qt contracts use the offscreen platform, render/interaction checks use Xvfb,
and native checks start their own headless Weston with Mesa software GL.
No host display socket, device, Pictures folder or app profile is mounted.
The Wayland startup artifact separates daemon startup, Qt/QML, first frame
and media readiness, with a warmup and five measured launches. Its 3-second
warm first-frame limit catches severe regressions on shared software-rendered
runners; it is not the desktop GPU target or a cold OS boot measurement.
Compare desktop launches separately using
[the startup method](../docs/startup-performance.md).

`PINHOLE_CI_CACHE`, `PINHOLE_CI_OUTPUT`, `PINHOLE_CI_CPUS` (default 2) and
`PINHOLE_CI_MEMORY` (default 7g) customize the local run. The container runs with the invoking user's uid/gid and a private temporary
home. The image's account records are mapped to that UID with this private
home; host account files are never mounted. Dedicated cache and output
directories remain owned by that user. Compilation uses
`CARGO_BUILD_JOBS` and `PINHOLE_BUILD_JOBS` to stay within the job's memory budget.
