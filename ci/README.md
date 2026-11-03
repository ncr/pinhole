# Reproducible checks

GitHub Actions runs the same commands as a local checkout. Docker is the
only host dependency; no packages, windows, clipboard, library or service
are installed on the host by these checks.

```sh
ci/run compiler blaze   # bootstrap and verify the reviewed compiler
ci/run rust blaze       # Rust tests + independent JPEG/SIMD parity
ci/run rust cargo       # explicit reference compiler, isolated targets
ci/run ui blaze         # Qt, query/accessibility/frame contracts, plugins, installer
ci/run visual blaze     # Xvfb rendering and real application interactions
ci/run native blaze     # private headless Wayland compositor, startup and scrolling
ci/run all blaze
```

The Docker base is pinned by digest and its Arch packages by archive date
in `Dockerfile`. Change these deliberately together with the compiler pin.
Every job records its exact package versions, compiler, command, duration
and exit status, retaining logs even on failure. Results are written under
`.ci-results/<mode>-<builder>/` (ignored by Git); CI uploads them as artifacts.
The source mount is read-only. A private source clone receives tracked and
unignored working files so local checks include uncommitted changes;
credentials and ignored build output are never copied into it.

The compiler cache key includes the environment, full source pin, patch
and bootstrap scripts. It has no partial-key fallback. The compiler job
primes this cache before dependent jobs. A fresh compiler bootstrap uses
Cargo only to build the reviewed Blaze compiler, then validates JPEG and
SIMD results before activation. Application builds use Blaze throughout;
the separate Cargo job is an explicit correctness reference, never an
automatic recovery path. See [the compiler notes](../docs/blaze.md).

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
home. Dedicated cache and output directories remain owned by that user and
are not writable by other users (Blaze rejects unsafe daemon ancestors).
Do not point these variables at personal data. Compilation uses
`PINHOLE_BUILD_JOBS` to stay within the job's memory budget.
