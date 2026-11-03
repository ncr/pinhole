# Photo-search ANE follow-up — 2026-11-03

**The M2 Pro ANE is now running.** The first image projection of both
candidate models compiles through a new decomposition and passes checks
on the actual device. This supersedes the initial firmware/device blocker
in [the benchmark notes](photo-search-benchmark.md). It does not mean the
complete models or semantic search are implemented.

## Recent upstream work

Checked publishers' releases and commit dates on 2026-11-03:

- [omarchy-ane 0.5.9, November 3](https://github.com/joshuaswarren/omarchy-ane/releases/tag/v0.5.9):
  M2 Pro/T6020 is enabled by default, backed by passing DKMS and in-tree
  hardware records. The other prominent new work concerns M3 bring-up.
- [omarchy-mlx 0.7.36, November 3](https://github.com/joshuaswarren/omarchy-mlx/releases/tag/v0.7.36):
  fixes `install.sh --ane` on aarch64 by shipping the missing tmpfiles policy.
  [0.7.37, also November 3](https://github.com/joshuaswarren/omarchy-mlx/releases/tag/v0.7.37)
  primarily fixes GPU long-context prefill, rather than adding CLIP/SigLIP ANE execution.
- [mil-hwx-compiler 0.3.2, November 2](https://github.com/joshuaswarren/mil-hwx-compiler/releases/tag/v0.3.2):
  maintenance/documentation release, with compiler behavior unchanged from
  0.3.1. The October 25 release adds two specific H14 matrix geometries.
  Source at `296d7ac6ebddc15967b071183dcf40bc41d3891b` still requires decoded
  shapes for H14 convolution and matrix multiplication. It cannot directly
  accept the candidate patch convolutions.
- [ANEForge](https://aneforge.readthedocs.io/en/stable/getting-started/)
  has pretrained CLIP/ViT loaders, but its runtime requires macOS. Its
  [FAQ explicitly excludes Asahi Linux](https://aneforge.readthedocs.io/en/stable/faq/).
  It is not a drop-in runtime for this Linux session.

No ready, verified end-to-end CLIP/SigLIP deployment for this Linux stack
was found in those sources. The news is real; driver availability, compiler
coverage and complete model support are separate milestones.

## Device setup actually performed

The existing kernel already provides `ane_t6021`, so no DKMS package,
replacement module or boot overlay was installed. The userspace tools were
built locally from `omarchy-ane` revision
`91ceff571e48aa1ad724ae0965be6edc96686d14`:

```sh
make -C .tools/omarchy-ane/tools ane-run
```

The upstream firmware fetcher downloaded only the ANE member from Apple's
pinned 13.5 IPSW. The 5,004,072-byte payload matched SHA-256
`a9c4b771294a6b115624d9480a6248d0899a1681a575e865070b87a3248427bc`.
It was staged in `.tools/` before installation.

There was no running graphical polkit agent. A visible terminal allowed
system authentication for installing that one file under
`/usr/lib/firmware/apple/ane/` and reprobed `284000000.ane` through the
normal platform-driver interface. The existing module bound successfully
and exposed `/dev/accel/accel0`. No reboot, module reload, kernel rebuild,
boot-file edit or direct register write was performed.

The upstream add fixture passed **20/20 calls bit-exact**, median 0.344 ms.
A single-row 1024×1024 matrix using actual CLIP projection weights then
passed all 1024 lane checks. These preceded larger projection submissions.

## The local workaround

[`bin/photo-search-ane`](../../bin/photo-search-ane) leaves the compiler's
hardware envelope intact. Instead, it lowers the patch projection into
already supported H14 matrix multiplications:

1. Extract each non-overlapping RGB patch in channel/row/column order.
2. Normalize each output filter with an exact power-of-two scale.
3. Zero-pad the output width to 1024. Tile the reduction into 1024-wide
   pieces and image patch rows into supported sizes 64, 8, 2 and 1.
4. Compile the tiles using the existing `mil-hwxc --target H14`.
5. Run the tiles on ANE. Sum reduction partials in fp32 on the host,
   reverse each filter's scale, trim padding and add the original bias.

Patch extraction and the final correction run on the CPU; the large
constant-weight matrix multiplications run on ANE. This is explicitly a
hybrid projection, not a claim that every operation executes on the NPU.

Initial **unscaled** attempts failed the upstream per-lane check: one lane
at K=1024 reached 4.020 condition units; a K=256 attempt reached 6.040.
Shortening the reduction alone did not solve it. The failures were concentrated
in small filters. Power-of-two filter normalization solved the observed
failures without changing either the per-lane limit or the full-projection
acceptance thresholds. Low-magnitude arithmetic is a suspected cause, not
a proven diagnosis of the hardware's internal rounding/underflow behavior.

Each ANE call is checked by the existing runner against fp64 products of
its actual fp16 inputs and weights: 2 ulp **or** 4 condition units per
lane, and zero padding. Any failed dispatch/check stops the run, preserves
its diagnostic inputs/output locally and produces no CPU fallback result.
The entire reconstructed projection is independently compared against
an fp32 convolution using the original, unscaled publisher weights.

The checker reads and reverses the **actual ANEC constant permutation**,
checking the reconstructed weight bytes against their recorded hash. It
does not assume that supplying the correct MIL means the compiled constants
are correct. Input/output manifests must have the expected dense fp16
H14 channel layout before device execution.

## Reproduce

Optional experiment dependencies are isolated from Pinhole's runtime:

```sh
uv venv --python /usr/bin/python3 .tools/photo-search/venv
uv pip install --python .tools/photo-search/venv/bin/python \
  numpy==2.3.4 pillow==12.0.0
```

For each model (`siglip2` or `clip`):

```sh
.tools/photo-search/venv/bin/python bin/photo-search-ane fetch-projection \
  --model clip --output .tools/photo-search/projection-weights/clip

.tools/photo-search/venv/bin/python bin/photo-search-ane lower \
  --weights .tools/photo-search/projection-weights/clip \
  --compiler /path/to/mil-hwxc \
  --output .tools/photo-search/projection-scaled/clip
```

The download command reads only the safetensors header and projection
tensors from immutable model revisions. Exact bounded HTTP ranges are
mandatory; a server returning the full checkpoint is rejected before reading
its body. Approximately 2.4 MB was fetched for SigLIP and 9.5 MB for CLIP,
rather than their complete checkpoints. It records local subset hashes;
this is not verification of the entire checkpoint's checksum.

The default verification backend is **CPU simulation**, with that label
in its report. Actual device verification is explicit:

```sh
OPENBLAS_NUM_THREADS=2 .tools/photo-search/venv/bin/python \
  bin/photo-search-ane verify --backend ane \
  --runner .tools/omarchy-ane/tools/ane-run \
  --smoke-receipt .tools/photo-search/ane-smoke.json \
  --weights .tools/photo-search/projection-weights/clip \
  --plan .tools/photo-search/projection-scaled/clip \
  --corpus .tools/photo-search/icloud-pl-v1/corpus.json --samples 4 \
  --output .tools/photo-search/projection-scaled/clip/verification-ane.json
```

Device mode is gated on a bound M2 Pro driver and a passing upstream
20-call add-fixture receipt, annotated with `boot_id` read immediately after
the smoke test from `/proc/sys/kernel/random/boot_id`. A receipt from another
boot is rejected before dispatch. No device mode silently falls back to CPU.
For an extracted compiler package, set `LD_LIBRARY_PATH` as documented in
[the initial notes](photo-search-benchmark.md).

## Measured result and remaining work

Both models used one deterministic-noise control and four local thumbnails.
Each path saw identical diagnostic pixels; these resize/normalization steps
are **not** asserted to be the complete checkpoint preprocessing pipeline.

| Actual ANE test | SigLIP 2 projection | CLIP projection |
|---|---:|---:|
| Unique compiled programs | 2 | 6 |
| Calls per image | 5 | 21 |
| Checked calls across five cases | 25/25 | 105/105 |
| Maximum relative L2 error vs original fp32 convolution | 0.000225691 | 0.000473959 |
| Minimum cosine vs original fp32 convolution | 0.9999999745 | 0.9999998886 |

The predeclared full-projection thresholds were relative L2 ≤0.002 and
cosine ≥0.99999; they were not widened after the failed attempts. These
are projection-output comparisons, **not final image embedding scores**.
Only execution time inside each runner call is recorded; spawning processes,
loading programs, input preparation, fp64 checking and reference inference
make this harness unsuitable for claiming indexing throughput or a speedup.

The [aggregate receipt](photo-search-ane-results.json) contains source/compiler/
firmware identities and report hashes without photo ids or boot identifiers.
Private diagnostics remain under `.tools/photo-search/` and were not uploaded.

This projection experiment was followed by the
[full-encoder hybrid benchmark](photo-search-full-encoder.md). Its corrected
CLIP run completes the vision encoder and Polish query rankings, with an
explicit CPU/ANE operation boundary. Retrieval quality remains unjudged.

Offline regression tests:

```sh
PINHOLE_TEST_ANE_COMPILER=/path/to/mil-hwxc \
  .tools/photo-search/venv/bin/python -m unittest discover -s bin/tests \
  -p test_photo_search_ane.py -v
```

These test bounded downloads, RGB/patch order, row coverage, actual compiler
constants, scaling/bias, corruption detection, stale smoke rejection and
stopping on a failed dispatch without fallback. They never access a device.
All **75** Python script tests passed with this experiment's NumPy environment
and the real compiler integration enabled, including all **9** new ANE tests.
