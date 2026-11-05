# Full image encoders on the M2 Pro

The offline prototype now runs the complete published CLIP ViT-B/32 and
SigLIP 2 base/16-224 encoders, with an explicit hybrid ANE backend. It uses
the frozen 384-thumbnail corpus and 40 Polish queries described in
[the benchmark protocol](photo-search-benchmark.md). No private images,
vectors, rankings, photo ids or library keys are committed or uploaded.

## What actually runs where

`examples/semantic-search/engine/photo-search-models encode --backend hybrid-ane` offloads attention
q/k/v/out linear projections and CLIP's final visual projection to the ANE.
It keeps the patch convolution, MLP, layer normalization, dynamic attention
products, softmax and SigLIP pooling on CPU. These boundaries are selected
before inference and recorded in `run.json`; there is no runtime CPU fallback.
The text tower is CPU: the multilingual sentence-transformers CLIP adapter
for CLIP, and the publisher's lowercase/64-token recipe for SigLIP 2.

The complete checkpoints are pinned by revision and verified against their
publisher safetensors SHA-256 values. Image preprocessing uses the local
publisher processor (CLIP bicubic/center crop; SigLIP bilinear/224 square),
after EXIF orientation and ICC-to-sRGB conversion. Downloads are a separate
step; inference sets offline flags and never fetches missing models.

The resident backend emits only the independently validated H14
64×1024×1024 matrix geometry. It pads rows and both weight axes, uses
power-of-two weight normalization, unscales partial sums and adds biases
on the host. Actual compiled constants are unpacked and checked before
cache admission. It caps resident program count and buffer memory, locks
compilation/device access and rejects a changed compiled cache.

## Measured outcome

The [aggregate receipt](photo-search-full-encoder-results.json) records
numerical agreement and timings. These are local prototype measurements,
with two CPU threads, and not retrieval-quality scores.

| Corrected runs | CLIP | SigLIP 2 |
|---|---:|---:|
| Completed photos | 384/384 | 384/384 |
| Final embedding cosine vs CPU, minimum | 0.9999978914 | 0.9999936282 |
| Final embedding cosine below 0.99999 | 0/384 | 0/384 |
| Same top-10 sets for Polish queries | 39/40 | 40/40 |
| Same top-10 order | 38/40 | 37/40 |
| CPU median seconds/photo | 0.0993 | 0.3005 |
| Hybrid median seconds/photo | 0.1491 | 0.3983 |
| Actual ANE calls | 18,816 | 73,728 |
| Distinct programs numerically checked | 49/49 | 48/48 |

Every first execution of a compiled program is checked with the upstream
2-ulp OR 4-condition-unit per-lane criterion; scoped diagnostics also use
`--check all`. The complete corrected run uses `--check first`, not a
per-call numerical check. Inputs/outputs must always be finite and padding
must remain zero. NPU execution alone took 13.82 seconds for CLIP and 37.74
seconds for SigLIP 2. End-to-end time also includes preprocessing, host
operations and transfers. Both hybrid runs were slower than their CPU
references. The two hybrid models shared a mean of 2.85 of their first
10 results per query. Relevance judgments are still needed to compare
which results fit the Polish descriptions better.

The initial all-ANE patch-convolution experiment passed each matrix check
but accumulated too much downstream error on several screenshot inputs:
CLIP had 50/384 final cosines below 0.99999 (minimum 0.99875). Keeping that
first convolution in fp32 on CPU fixed these outliers. The old results
remain recorded as an experimental recipe; they are not substituted for
the corrected run. CPU patch projection is now the default fixed policy.

## Interrupted SigLIP run and device health

The corrected SigLIP run logged 56 photos, saving a 32-photo checkpoint,
then firmware returned `EXCH op=0x204 failed -110` and a legacy channel
timeout. At that point the device reported 127,531 jobs over all experiments
and 271,450,112 buffer bytes. The cause is not established. The checkpoint
had minimum final cosine 0.9999990014 across the saved 32 photos. No further
device work was submitted during that boot.

After the November 4 reboot into kernel `7.2.11-14-1-ARCH`, the upstream
add fixture passed 20/20 calls bit-exact with a 0.338 ms median. A fresh
384-photo run then completed with 73,728 successful submissions. It used
the same CPU-patch recipe throughout. The earlier partial checkpoint lacked
per-photo timings, so the comparison above uses the fresh complete run.

The upstream driver retains failed allocations until reboot and treats a
hung device as untrustworthy for the rest of the boot. A local health marker
therefore blocks new device submissions before loading libane. Restart the
machine and repeat the upstream smoke test to create a new current-boot
receipt before continuing hardware work. Do not reload the module, clear
the marker to retry, or silently complete the benchmark on CPU.

## Reproducing

Use the optional experiment environment with NumPy, Pillow, PyTorch,
transformers and sentence-transformers. Tested versions: Python 3.14.9,
NumPy 2.3.4, Pillow 12.0.0, torch 2.9.1+cpu, transformers 4.57.1,
sentence-transformers 5.1.2. Public model revisions are in
`examples/semantic-search/engine/photo_search/models.py`; models live under `.tools/photo-search/models`.

```sh
.tools/photo-search/venv/bin/python examples/semantic-search/engine/photo-search-models encode \
  --model clip --backend cpu --models .tools/photo-search/models \
  --corpus .tools/photo-search/icloud-pl-v1/corpus.json \
  --output .tools/photo-search/example-cpu
```

For hardware, select `--backend hybrid-ane`, and provide `--compiler`,
`--library` (the locally built libane.so), `--smoke-receipt` and `--cache`.
The cache's parent and the smoke receipt's parent are checked for a
current-boot `ane-device-health.json` quarantine marker. `--patch-backend
ane` is diagnostic only. `--diagnose --indices 0,1` saves CPU references and
per-projection comparisons for at most eight images. A failed run keeps
partial vectors but writes no successful run manifest.

`rank --images RUN --judgments JUDGMENTS --models MODELS --output FILE`
creates complete frozen-corpus rankings. `compare --runs ROOT --output FILE`
requires both complete CPU and hybrid runs for both models. Ranking overlap
only measures agreement: there are still zero human relevance judgments,
so neither model has earned a retrieval-quality recommendation.

Offline tests use synthetic models and optional real compilation, without
opening the accelerator:

```sh
PATH=/usr/bin:$PATH OPENBLAS_NUM_THREADS=2 \
  PINHOLE_TEST_ANE_COMPILER=/path/to/mil-hwxc \
  .tools/photo-search/venv/bin/python -m unittest discover -s bin/tests
```
