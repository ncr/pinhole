# Polish photo-search benchmark

This is development tooling, not an application search feature. No ML runtime
or model is loaded by the Pinhole window. Run the commands below from the
checkout root. The preparation and scoring commands need only Python's
standard library.

## Prepare a private corpus

```sh
python3 bin/photo-search-benchmark prepare \
  --library /path/to/pinhole/library \
  --output .tools/photo-search/icloud-pl-v1 --count 384
```

The source directory contains `library.sqlite` and cached thumbnails. The
tool opens SQLite read-only, including its current WAL, and copies existing
still-photo thumbnails. It does not start pinholed, contact iCloud, read
originals or alter the library. Videos are excluded; Live Photo stills are
included. Missing thumbnails and paths escaping the source directory are
excluded. The output must be a new directory outside the source library.

Sampling is deterministic for a given library snapshot and seed. It takes
turns between strata of capture year, original format and portrait/landscape
orientation, choosing within each stratum by a seeded hash of the asset id.
This deliberately includes rare strata: it does **not** estimate the frequency
of scenes in the entire library. Scene coverage and near-duplicate leakage
still require visual review. Dates in the library are Unix milliseconds.

`corpus.json` identifies copied bytes by SHA-256 and gives them opaque ids.
It records the sample procedure and corpus identity. `judgments.json` starts
with [40 Polish queries](photo-search-queries-pl.json), all unjudged. Review
the sample and freeze the queries before inference. Avoid tuning queries
to favor one candidate. The `.tools/` directory is already ignored by Git;
private photos, rankings and judgments stay there. The corpus directory
is created with mode 0700.

## Judge and score

In each query's `relevance` object, use the corpus photo id as the key and
an integer grade: 0 irrelevant, 1 partly relevant, 2 directly relevant.
After reviewing the **entire corpus for that query**, set `complete` to
`true`; omitted photos then mean irrelevant. Keep incomplete queries false.
Ground truth must be judged independently of a model's ranking.

A model runner must produce this format, with the exact corpus identity,
query text and **all** photo ids in descending similarity order per query:

```json
{
  "schema": 1,
  "corpus_id": "from corpus.json",
  "queries": {"pl01": "rodzina przy stole"},
  "rankings": {"pl01": ["first-photo-id", "second-photo-id"]}
}
```

The example is shortened; actual files must contain all queries and photos.
Record model/processor revisions, runtime versions, preprocessing, precision,
backend and timing separately alongside the rankings. This tool does not yet
provide a model inference runner.

```sh
python3 bin/photo-search-benchmark evaluate \
  --corpus .tools/photo-search/icloud-pl-v1/corpus.json \
  --judgments .tools/photo-search/icloud-pl-v1/judgments.json \
  --rankings .tools/photo-search/icloud-pl-v1/siglip2-rankings.json \
  --output .tools/photo-search/icloud-pl-v1/siglip2-quality.json
```

Reports contain precision, recall, graded nDCG and reciprocal rank at 10,
per query and macro-averaged over completed queries with positive judgments.
No-positive queries and incomplete queries are listed separately and excluded
from these metrics. A wholly unjudged corpus has `macro: null`, not a quality
score. The scorer verifies copied image hashes and rejects stale query text,
wrong corpus identities, unknown/duplicate ids and truncated rankings. Files
are hashed in the report so later corrections to judgments remain visible.

## ANE feasibility check — 2026-11-03

The current machine is an **Apple MacBook Pro 16-inch, M2 Pro (2023)**,
running Arch Linux ARM, kernel `7.2.11-13-1-ARCH`. This differs from the
Ryzen/RTX machine in the initial research. Hardware was identified from the
device tree, not inferred from the architecture alone.

The in-tree `ane_t6021` module is loaded and matches `apple,t6020-ane`.
It is not bound to the ANE platform device, and no `/dev/accel` node exists.
The kernel log reports missing
`apple/ane/t602x_ane0_fw_selene_rc4x.macho` and probe failure `-ENOENT`.
The stub firmware version is 13.5. The system has neither the `omarchy-ane`
userspace package nor the model compiler installed. Installation was attempted
but did not obtain system authentication; the pending request was cancelled.
No packages or firmware were installed, no module was reloaded, and no
accelerator task was submitted.

This is not a macOS/Core ML prediction environment. Apple documents that
[Core ML prediction requires macOS](https://apple.github.io/coremltools/docs-guides/source/model-prediction.html).
The Linux path is [omarchy-ane](https://github.com/joshuaswarren/omarchy-ane)
plus [mil-hwx-compiler](https://github.com/joshuaswarren/mil-hwx-compiler),
with its own operator/shape restrictions. A working GPU or an import of MLX
would not establish ANE execution.

The repository's available `mil-hwx-compiler 0.1.0-1` package was downloaded
and extracted **inside `.tools/`**, without installing it. Its SHA-256 matches
the local pacman sync database:
`2566e8999f84c3aab0269c343a115e6bd9dd256a14dc3cf27c78545c1841e48f`.
Compiler executable SHA-256:
`741dce3200be2805f3be27da16c5d13d6b68c0426b62dd40fa0d5360dab39083`.

```sh
python3 bin/photo-search-benchmark probe-ane \
  --compiler /path/to/mil-hwxc --output .tools/photo-search/ane-probe
```

For an extracted package, also set `LD_LIBRARY_PATH` to its
`usr/lib/mil-hwx-compiler/lib` directory. The probe generates valid MIL with
synthetic zero-weight blobs and runs **only the compiler**, targeting H14:

| Probe | Geometry | Observed result |
|---|---|---|
| Control add | Two fp16 `[1,512,1,1]` inputs | Compiles, exit 0 |
| SigLIP 2 patch projection | `[1,3,224,224]`, 768 filters, kernel/stride 16 | Exit 65, `h14.conv-outside-envelope` |
| CLIP ViT-B/32 patch projection | Same input/channels, kernel/stride 32 | Exit 65, `h14.conv-outside-envelope` |

These are geometry probes, **not full model exports or inference results**.
They show that direct lowering of the initial convolution to this packaged
compiler is blocked. They do not prove that an alternative decomposition
or another compiler cannot work. Installing firmware alone cannot remove
this compiler limitation. Extending/decomposing these operations, validating
the remaining encoder operations and comparing embeddings against a reference
are prerequisites for an honest ANE comparison. No CPU fallback was used as
an ANE result.

Pinned candidate revisions resolved from the publishers' model APIs:

| Model | Revision |
|---|---|
| `google/siglip2-base-patch16-224` | `75de2d55ec2d0b4efc50b3e9ad70dba96a7b2fa2` |
| `sentence-transformers/clip-ViT-B-32` (image encoder) | `327ab6726d33c0e22f920c83f2ff9e4bd38ca37f` |
| `sentence-transformers/clip-ViT-B-32-multilingual-v1` (text encoder) | `58edf8cada9e398793dca955574a48cbb7f18be2` |

The private corpus has **384 stills**, **99 represented strata**, capture
years **2001–2026**, sampled from **46,286** eligible local still thumbnails;
none was missing. All copied image hashes were verified. There are 40 Polish
query templates and **zero completed judgments**. No model weights were
downloaded, no private image was submitted to an external service, and no
quality, inference-speed or NPU-use result is claimed.

The preparation/scoring contracts are exercised with synthetic data by
`python3 -m unittest discover -s bin/tests -p test_photo_search_benchmark.py`.
