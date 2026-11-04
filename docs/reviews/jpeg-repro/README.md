# Standalone JPEG and SIMD parity regression

No Pinhole code, private photos or libultrahdr. Run from the repository root:

```sh
cargo run --release --locked --manifest-path docs/reviews/jpeg-repro/Cargo.toml -- /tmp/pinhole-jpeg-simd
cargo run --release --locked --manifest-path docs/reviews/jpeg-repro/Cargo.toml --no-default-features -- /tmp/pinhole-jpeg-scalar
```

The Cargo checks exercise the decoder with its default SIMD features and
again with those features disabled. With an output directory each run also
decodes and writes all seven fixtures. CI uses temporary output directories.

`gray-ramp.jpg` is the original 96×64 grayscale ramp encoded by image 0.25.10
at JPEG quality 90. Each row is `x * 255 / 95`; the decoder must stay within
2 levels of that ramp (both healthy compilers currently have max error 1).

The six fixtures in `fixtures/` were generated with Pillow at JPEG quality 91.
Their names record mode, dimensions, chroma subsampling (`s0` 4:4:4, `s1`
4:2:2, `s2` 4:2:0), and progressive flag. For coordinate `(x,y)`, RGB values
are `((23*x+11*y)%256, (7*x+37*y)%256, (51*x+3*y)%256)`; grayscale is
`(23*x+11*y)%256`. They cover color, grayscale, progressive coding, odd sizes
and width one. Pillow is needed only to regenerate fixtures, not run tests.

The same executable independently verifies saturating x86 packing of
signed/unsigned i32→i16 and i16→i8 values, preserving each 128-bit lane.
SSE2 is baseline on x86-64; SSE4.1, AVX2 and AVX512BW tests run when the CPU
supports them. ARM hosts run only JPEG checks.

On 2026-11-03 upstream Blaze `0.20261030.1` failed the first SSE packing
assertion and decoded the ramp as repeated pairs (`0,2,0,2,10,13,10,13,…`),
with max error 255. Cargo produced `0,2,5,8,10,13,16,18,…` with max error 1.
Pillow independently confirmed the correct ramp. The cause was the compiler's
`pack_lanes` loop taking half of each 128-bit input lane. Pinhole's reviewed
compiler patch repaired that loop. The comparison is historical.
See [the original setup notes](https://github.com/ncr/pinhole/blob/3bfdc6f/docs/blaze.md).

`set_use_unsafe(false)` still enables some SIMD in zune-core 0.5.1 because
its per-architecture feature flags are ORed with the unsafe flag. The scalar
check therefore disables the zune-jpeg SIMD Cargo features at compile time.
