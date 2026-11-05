# Offline semantic index worker

`examples/semantic-search/engine/photo-search-worker` owns only a rebuildable vector index. For the
shared library, pinholed supervises the worker, which reads the current
thumbnail catalog through a read-only SQLite connection. It checks source
ownership and reads rows in a single transaction. For a separate library
the window supplies the catalog through the public `pinhole.query` API.
Neither path fetches originals or contacts a model service.
See [the full-encoder measurements](photo-search-full-encoder.md) for the
explicit CPU/ANE boundary and the current firmware timeout limitation.

The helper is separate from the GUI, and models are loaded lazily. The
selected encoder accepts Polish or English queries. The socket client uses
only Python's standard library, so status polling does not import PyTorch.
The worker can answer queries over saved ANE vectors while the device is
quarantined, since the text tower runs on CPU. Starting new indexing fails
before model/device loading in that boot; there is no backend fallback.

## Protocol

Start the server with the experiment's Python environment:

```sh
.tools/photo-search/venv/bin/python examples/semantic-search/engine/photo-search-worker serve \
  --index /private/cache/index.sqlite --library /path/to/library \
  --library-key source-key --models .tools/photo-search/models \
  --model clip --backend cpu
```

A backend is required explicitly. `hybrid-ane` additionally requires
`--compiler`, `--ane-library` and `--smoke-receipt`, plus a `--cache` for
compiled programs. Model pins, the processor hash, model recipe, backend,
vector dimension and source key form the persistent index identity. An
existing database with another identity is rejected **before any writable
SQLite connection or pragma**, including an accidentally supplied library
DB. Nothing converts another application's database into an index.

The server prints a JSON `ready` event and errors on stdout. The shared
worker follows the daemon's lifetime, the current source and the plugin's
enabled switch. It reads the persisted pause setting even with no window
connected. A window owns its separate library's worker with `pinhole.run`.
Finished vectors are committed per
photo in WAL mode. SIGTERM is handled between operations and the socket is
removed on orderly exit. A process lock rejects a second worker before it
can unlink a live socket or open the same index.

Requests use one JSON object per Unix-socket connection, ending with a
newline. `request --index INDEX` computes the same short socket path and
reads the request from stdin; `--socket PATH` is also available. The default
socket lives in an owner-checked mode-0700 temporary directory with a
mode-0600 socket. Index files are mode 0600. Requests are bounded to 24 MiB;
responses to the CLI client to 1 MiB. This is process ownership and privacy,
not a sandbox against arbitrary code running as the same account.

Operations:

- `catalog`: `{assets:[{id,path},…]}`, at most 100,000 unique stable photo ids,
  with thumbnail paths relative to the library directory. Absolute paths,
  symlink escapes, duplicates and excessive input are rejected. Missing
  thumbnails are individual indexing errors. Removed photos lose derived
  vectors; changed paths invalidate old results. With `auto_index: true`,
  it queues missing or invalidated vectors. With `auto_index: false` it
  pauses the queue.
- `start`, `stop`, `status`: indexing proceeds one photo at a time between
  requests. It hashes local thumbnail bytes and skips unchanged embeddings.
  Restarting resumes saved work. A model/device failure terminates the job
  and reports partial progress without claiming completion.
- `query`: `{text:"pies na plaży",limit:100}`, at most 2048 characters and
  500 results. Normalized image/text vectors use exact dot-product ranking,
  with stable photo-id ordering on ties. Similarity is not a probability.
- `suggest_tags`: `{id:"photo-id"}` returns up to five `{id,label}` pairs
  from a fixed Polish vocabulary. It checks only the selected thumbnail and
  reuses its saved image vector. The resident text encoder embeds the
  vocabulary once per worker. Missing or stale vectors return an empty
  list. No image inference is required for suggestions.
- `import`: `{run:"/path/to/completed-run",corpus:"/path/to/corpus.json"}`.
  The worker requires a completed encoder run, matching model/processor/
  backend policy and dimensions, and maps opaque benchmark ids through
  the plugin-supplied catalog. It verifies every current thumbnail hash
  before saving any imported vector.

Before ranking, indexed files are revalidated against their content hashes;
a resident stat cache avoids rehashing unchanged files. Same-path replaced,
missing or escaped thumbnails are excluded until reindexed. The first query
after restarting rechecks their hashes. A source change opens another
index rather than mixing source ids.

The derivative store tests cover restart, source identity, foreign database
preservation, stable ties, malformed embeddings, catalog removal, same-path
replacement, missing/symlinked thumbnails, cache skips, terminal inference
failure, quarantine, benchmark import and persistent text-tower reuse.
They use synthetic data and never access an accelerator.

## Adding an engine

`examples/semantic-search/engine/photo_search/engines.py` owns the available engines and host profiles.
Register an engine with its model dimensions, index identity, image and
text encoders, resource cleanup, and any indexing availability checks.
Add a profile with an interpreter, worker arguments, and a unique index
filename for the recipe. The search card receives this configuration from
the helper. The catalog, index store, ranking, and UI use the same protocol
for every engine.
