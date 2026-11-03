# Rust rebuild comparison

These are historical speed measurements, before the JPEG compiler fix and
the input-change worker guard. They do not certify runtime correctness or
the speed of the current guarded rebuild path. The current pin and
regression gates are documented in [blaze.md](blaze.md).

Measured on 2026-11-02 on this development machine, at Pinhole revision
`42f68ffbeeb6d458d4cfbdd04525c21f63829c51`.

- Blaze `0.20261030.1` (`6560553`); Cargo `1.99.1` (Arch Rust `1:1.99.1-1`).
- Same source copy, core workspace, `build --release --locked --offline --workspace`.
- Cargo used its own `--target-dir`; Blaze used the copy's `core/target`.
  The installed checkout and its binaries were untouched.
- Both compilers were warmed up before timing. Existing compiler caches,
  including Blaze's shared function cache, were kept. These are developer
  rebuild measurements, not clean builds or first-time installation times.
- Three runs per scenario, alternating compiler order. Times are external
  wall-clock measurements, including process startup, rather than just the
  compiler's own reported time.

| Scenario | Blaze runs (seconds) | Cargo runs (seconds) | Median speedup |
| --- | --- | --- | --- |
| No source change | 0.057, 0.014, 0.010 | 0.113, 0.058, 0.058 | 4.3× |
| Daemon CLI default changed | 0.292, 0.085, 0.082 | 0.514, 0.435, 0.429 | 5.1× |
| Core library diagnostic changed | 0.227, 0.049, 0.036 | 6.278, 6.085, 6.057 | 123× |

The daemon edit changed `default_value_t = 8` to 9, 10 and 11 in
`core/daemon/src/main.rs`. The library edit changed a diagnostic string in
`forget_compatible` in `core/src/storage.rs`, using a different string each
round. Both are deliberately small edits. Blaze's logs confirm that it
rebuilt `augment_args` for the CLI edit and `forget_compatible` for the
library edit; Cargo rebuilt the affected crate and its dependents. Both
final binaries contain the changed library string, and `--help` confirms
the restored daemon default.

To reproduce, copy the tracked source tree outside the working installation.
First run each command once to prepare its outputs, then measure both after
identical edits:

```sh
bin/build-rust build --release --locked --offline --workspace --manifest-path core/Cargo.toml
PINHOLE_RUST_BUILDER=cargo bin/build-rust build --release --locked --offline --workspace \
  --manifest-path core/Cargo.toml --target-dir /tmp/pinhole-cargo-comparison
```

The large library-edit gain is specific to a one-function change and warm
caches. It does not imply that a whole installation is 123× faster. Qt/C++
building is separate and unaffected by the Rust builder. Runtime performance
and binary-size comparisons are outside this measurement.

## Current guarded compiler — 2026-11-03

The patched compiler and input-change worker guard were measured again on
one isolated current source copy. Both builders were warmed before sampling;
Cargo had its own target, Blaze its own worker/target and the pinned persistent
store. Three fresh changes to `storage::forget_compatible`'s diagnostic string
alternated builder order (B/C, C/B, B/C). Every resulting `pinholed` contained
the corresponding new marker. No production source or installed binary was
changed. Commands were `build --release --locked --offline --workspace`,
`CARGO_BUILD_JOBS=2`, Cargo `-j2`; codegen affinity remained the host default.
A separate CI container shared the host with a two-CPU quota.

| Scenario | Guarded Blaze, seconds | Cargo, seconds |
|---|---:|---:|
| Small core-library edit, three samples | 6.630, 6.787, 6.996 | 22.207, 20.755, 23.276 |
| Median of those edits | **6.787** | **22.207** |
| Unchanged source, two warm observations | 0.304, 0.219 | 0.079, 0.060 |

For this edit Blaze is **3.27× faster**, saving about 15.4 seconds. Its worker
restart prevents the observed stale-type defect and costs time; the earlier
123× result does not describe this path. Cargo is faster for the measured
no-op. These are narrow developer rebuild comparisons, not clean-install,
whole-project or runtime speed claims. The two no-op observations are not a
three-run alternating experiment. See the [raw edit samples](reviews/2026-11-03-guarded-builds.json)
and [compiler correctness/guard notes](blaze.md).
