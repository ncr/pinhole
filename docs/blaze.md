# Pinhole's Blaze compiler

`bin/build-rust` builds Pinhole through Blaze. It stops on a failed build;
Cargo is used only when explicitly selected for reference checks. Run
`bin/setup-blaze` to prepare the pinned compiler, also called by `bin/install`.

## Reviewed SIMD correction

The upstream release `0.20261030.1` has a code-generation defect in
`crates/hir-ty-codegen/mod.rs::pack_lanes`: it takes only `8 / wide_bytes`
elements from each input of a 128-bit lane. The x86 pack instructions take
`16 / wide_bytes`. This corrupts signed and unsigned saturating i32→i16 and
i16→i8 packing, including the zune-jpeg decoder used by Rust's image crate.
Changing JPEG options or using the LLVM backend does not repair this lowering.

Pinhole pins **0.20261030.1+pinhole.1**. `bin/blaze-source.json` records the
full upstream Git revision and SHA256 of `bin/blaze-patches/pack-lanes.patch`.
The patch changes the lane count and appends a distinct compiler version.
The compiler hashes its own sources into the generated-code cache identity;
Pinhole also isolates each version's compiled store and each checkout's daemon.
Immutable Rust toolchains can be shared. An old upstream installation stays
available beside the patched one, without mixing compiled objects.

`bin/build-rust` also gives each checkout a short, private daemon namespace.
Before compiling, `bin/run-blaze` hashes Rust sources, manifests, lockfiles,
Cargo configuration and compiler setup metadata. When those contents change,
it restarts the live worker while retaining the persistent compiled-artifact
store. Identical inputs retain the worker. A checkout lock serializes the
check and command; edits during a command stop its worker and reject the
result (exit 75 after an otherwise successful command), so the build must be
retried. A separate inode/mtime/ctime stamp also detects edits restored to
their original contents during the command. Cancelling the wrapper terminates
and reaps its foreground process group, then stops the worker while the lock
is still held. Generated trees and agent/tool state are excluded from this scan;
real untracked Rust source is included.

This guard follows an observed incremental-code defect during development:
after changing a query result from `u32` to `Option<u32>`, three integration
executables reached an invalid enum-discriminant trap. A preserved executable
reproduces it without concurrent builds; its core contains a bare RGB integer
where the Option tag belongs. Restarting the worker rebuilt the same target
correctly without clearing its persistent store. Tiny contextual-type
reproductions have not yet isolated the responsible upstream query. Tests
with unchanged sources passed 16 concurrent commands using shared and
separate targets; this evidence does not attribute the defect to concurrency.

`bin/setup-blaze` downloads the original pinned client and its Rust toolchain,
fetches the exact source revision, verifies and applies the patch, and obtains
the upstream pinned LLVM archive with SHA256 verification. Cargo bootstraps
**the Blaze compiler** once; Pinhole continues to compile through Blaze.
Before activating it, setup runs the independent SIMD/JPEG regression. A
provenance file records the pin and engine checksum; later setup runs verify
it before reuse. Setup serializes concurrent executions and activates the
compiler only after this regression passes.

On Linux the bootstrap needs Python 3 (including lzma), Git, curl, tar/gzip,
a C/C++ toolchain, make and pkg-config. No system LLVM installation is needed.
The initial local build took about nine minutes with two jobs, with a 1.7 GB
release target and a 352 MB LLVM prefix. Temporary bootstrap outputs are
removed after success; the final engine is about 218 MB, plus the original
client binaries and shared Rust toolchain. Setup uses one job by default for
7 GB CI hosts. Set `PINHOLE_BLAZE_BOOTSTRAP_JOBS=2` for a larger machine or
`PINHOLE_KEEP_BLAZE_BOOTSTRAP=1` when diagnosing the bootstrap. Cache the
installed tools and toolchains, keyed by both pin files and patch, rather
than retaining the whole source build.

On ARM64 Linux with kernel pages larger than 4 KiB, the upstream release
engine can exit with `jemalloc: Unsupported system page size` before it
can provide the Rust toolchain path. This bootstrap installs the release
launchers with toolchain setup deferred and uses `rustup` on PATH to obtain
the nightly named by the pinned Blaze source. It verifies the Rust commit,
rebuilds the engine with `JEMALLOC_SYS_WITH_LG_PAGE=16` (64 KiB allocator
pages), and runs the same JPEG regression before installing it. Rustup
adds the pinned nightly and its WASI target alongside the existing
toolchains. Pinhole still builds with the resulting Blaze engine.

On the M2 Pro with 16 KiB kernel pages, the rebuilt engine passed
`blaze --version` and the scalar JPEG reproduction. The required SIMD
reproduction stops on `aarch64.neon.sqshrun.v4i16`, called by zune-jpeg's
`vqshrun_n_s32`, with both Cranelift and LLVM. Setup leaves the engine in
staging and preserves the previous installation when this check fails.

## Correctness checks

```sh
bin/check-rust-parity
BLAZE_TESTS=all bin/build-rust test --release --workspace --no-fail-fast --manifest-path core/Cargo.toml
python3 -m unittest discover -s bin/tests
```

The parity command deliberately builds a standalone decoder with both Blaze
and Cargo in isolated temporary targets, and compares every decoded byte.
It covers grayscale and RGB, 4:4:4/4:2:2/4:2:0 chroma, progressive coding,
odd image dimensions and a one-pixel-wide image. A second build disables the
zune-jpeg SIMD features entirely; `set_use_unsafe(false)` alone does not do
that in zune-core 0.5.1. Direct x86 intrinsic tests cover signed and unsigned
packing at SSE2/SSE4.1, AVX2 and AVX512 widths when the CPU supports them.
See [the reproduction](reviews/jpeg-repro/README.md) for fixture details.

The synthetic parity checks cannot establish correctness of every compiler
operation or image format. Run the full core tests and retain the explicit
reference check when changing the compiler pin. ARM hosts run the JPEG checks
but do not execute the x86 intrinsic regressions.

## Existing derived data

The old compiler could also have persisted incorrect average colours and
Series features. The service repairs average colours from thumbnails already
on disk in its background sync worker, using two low-priority threads and
write transactions of at most 256 rows. A durable checkpoint survives
cancellation or restart; missing/damaged thumbnails remain pending for the
next round. The decoder version advances only when all pending thumbnails
have been decoded successfully. This does not fetch originals or thumbnails
from the network and does not change capture/addition dates or source data.

Series invalidates only its derived `likeness` and `found` cache tables once,
then makes corrected features on demand. Manual grouping lives in the
plugin's separate state and is preserved. The grid's displayed thumbnails
use the native C++ decoder, so there is no reason to delete the image files.

## Portable standard library

The patched release does not exist on upstream's release server. Setup bundles
Blaze's own standard-library sources at `share/blaze/library` beside the
engine, where its native discovery looks. It verifies the complete file/link
tree against upstream `library/DIGEST` at the pinned source revision
(`f1524885a07419549a4f11439af96f991e69629cefb51269b971571227b3aa77`).
The approximately 79 MB bundle survives relocation and independent daemon
homes; it does not depend on the temporary bootstrap checkout or an inherited
`BLAZE_LIBRARY` variable. The parity gate explicitly removes that override.

Previously verified binaries missing this packaging component are repaired
from a verified cached source tree, or the exact upstream version's source
archive, without rebuilding LLVM/the engine. A corrupt source download is
rejected before replacing the existing bundle. Setup verifies both engine
and standard-library checksums before reuse; provenance records both.
