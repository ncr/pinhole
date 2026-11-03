# Standalone JPEG parity reproduction

No Pinhole code, no private photos, no libultrahdr. `gray-ramp.jpg` is a
synthetic 96×64 greyscale JPEG made with image 0.25.10's JPEG encoder at
quality 90. Each row is the integer ramp `x * 255 / 95`.

From the repository root:

```sh
CARGO_TARGET_DIR=/tmp/pinhole-jpeg-blaze bin/build-rust run --release --locked --offline \
  --manifest-path docs/reviews/jpeg-repro/Cargo.toml

PINHOLE_RUST_BUILDER=cargo bin/build-rust run --release --locked --offline \
  --manifest-path docs/reviews/jpeg-repro/Cargo.toml \
  --target-dir /tmp/pinhole-jpeg-cargo
```

On 2026-11-03, Blaze 0.20261030.1 on x86-64 produced repeated pairs of
pixels: `0, 2, 0, 2, 10, 13, 10, 13, …` instead of
`0, 2, 5, 8, 10, 13, 16, 18, …`. Cargo 1.99.1 decoded the ramp correctly,
as did Pillow's independent JPEG decoder. Both default SIMD and
`set_use_unsafe(false)` showed the difference under Blaze. This is not
evidence that disabling SIMD solves it.

The process exits nonzero when any channel differs by more than 2 from
the ramp. A healthy decode has at most 1 code of error. Keep the compiler
and the dependencies pinned when investigating; the exact responsible
compiler function is not yet isolated.
