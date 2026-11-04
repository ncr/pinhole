# Photo content search personal plugin

`examples/semantic-search` is a personal plugin for local English and Polish
text search over indexed thumbnails. It starts background indexing shortly
after Pinhole opens. Ctrl F opens the search card. The worker and model stay
off the first-frame path.

On a CUDA host the optional WeMM 4B environment selects the WeMM recipe.
Both image and text embeddings use the same resident BF16 model and the
original query language. A separate per-source index keeps WeMM vectors
apart from previous CLIP results. On the Mac the corrected CLIP hybrid
backend uses attention
projections on ANE, the first convolution and remaining operations on CPU,
and a resident multilingual CPU text tower. It is explicitly an experiment,
not the retrieval-quality winner or a measured speed improvement. See
[the measured results](photo-search-full-encoder.md) and
[the worker protocol](photo-search-worker.md).

## Dependencies and local setup

On Linux x86-64 with an NVIDIA GPU that supports native BF16 and about
11 GiB free VRAM, install `uv` and run from the checkout:

```sh
bin/setup-photo-search-wemm
```

Use `--snapshot /path/to/weights` to reuse an existing copy of the pinned
Tencent weights and tokenizer. Setup verifies their SHA-256 hashes.
The worker loads the local checkpoint when indexing or answering a query
against an existing index. It waits for about 11 GiB of free GPU memory
before loading WeMM for background indexing, and unloads the model on
pause or after one minute without inference when indexing is finished.

The host must support plugin API 8. On an Apple Silicon Mac running Linux, use the
environment and pinned models described in the full-encoder notes.
Both environments live under the checkout's ignored `.tools/photo-search`
directory. The plugin does not install packages or download models.

Keep this developer plugin in the checkout: its executable resolves its own
symlink with `readlink -f`, then finds the environment and worker relative to
the repository. It is not a standalone distributable folder. On an API-8
host, link the folder into the personal plugin directory and enable
**Photo content search** in Settings → Plugins:

```sh
ln -s /absolute/path/to/pinhole/examples/semantic-search \
  ~/.config/pinhole/plugins/me.semantic-search
pinhole plugin check me.semantic-search
```

Update the host with `bin/install --non-interactive` if its plugin API
predates API 8. The source installer builds the application with Cargo
and Qt. The personal plugin is linked and enabled separately as described
above.

## Using the prototype

Ctrl F opens a search card with up to 100 visible
results. Enter submits the description; Down moves focus to the results,
arrow keys move through the grid, and Enter opens the selected photo.
Closing that photo returns directly to the same results and selection.
Escape or a click outside the card closes search while indexing continues.
The plugin uses stable ids and filters other
plugins' hidden photos. Scrolling stays inside the shared `ScrollGrid`.

For the CLIP recipe, **Use the 384-photo benchmark** imports the existing corrected CLIP run
from `.tools/photo-search/full-v2/clip-hybrid-ane`. The worker maps its opaque
ids through a catalog obtained with `pinhole.query` and verifies current
thumbnail bytes before admission. Results explicitly cover indexed photos
only; importing this sample does not index the full library.

The footer shows saved tags and suggestions for the selected or hovered
result. Suggestions come from a fixed Polish vocabulary. Choose **Tag
selected photo** to edit the saved tags. Choose a
suggestion to save it. Choose it again or press its remove button to
remove it. Ctrl+Z and Ctrl+Shift+Z undo and redo edits. Escape returns
to search. Suggestions use the indexed image and the existing text tower,
so they also work with the imported sample during an ANE quarantine.

Enter a full saved tag name prefixed with `#` in the search field, such as
`#rower`. Enter `#` alone to see all tagged photos in the current source.
Tag matching ignores letter case and excludes hidden and removed photos.
It works with the search worker unavailable and reads only approved tags.

Approved tags are kept in the plugin's state by source and stable photo
ID, with a limit of 2,000 tagged photos per source in this prototype.
They survive index rebuilds. The editor works on the selected search
result. Tags are stored separately from photo file metadata. Suggestions
are ranked within the vocabulary and need human review.

Indexing works through the current source's local still thumbnails at low
CPU and I/O priority, with a pause between photos. **Pause indexing** saves
the pause across app restarts. **Resume indexing** picks up missing or changed
thumbnails. Completed vectors survive closing, reload and restart.
Each source has its own derivative cache and model identity. Library
refreshes reconcile pending work, keeping finished vectors and adding new
photos. Same-path thumbnail replacements are excluded before querying until
reindexed. Closing the card cancels its queries. Changing source cancels the
old worker and opens the new source's index.

A device timeout quarantines indexing until restart plus a fresh smoke test.
The existing imported index still accepts Polish queries entirely locally.
During quarantine the plugin explains how to restore device access in Polish.
The November 4 reboot and successful smoke test restored access on the
M2 Pro. The completed SigLIP 2 comparison used the same 384-thumbnail sample.
The CLIP import covers only the sample.

## Verification

Run the host checker:

```sh
app/build/pinhole plugin check examples/semantic-search
```

`examples/semantic-search/tests/run` builds an isolated fixture from an
existing local benchmark, copying only thumbnails and vectors. It substitutes
synthetic asset ids, separate profile/cache/Pictures paths and a fixture-local
quarantine marker, so its **indexing action never submits accelerator work**.
The photo service is explicitly unavailable on a fixture-private socket;
`PINHOLED=false` prevents launching a checkout or installed daemon, including
when the parent shell has service overrides. The real host, plugin, worker,
tokenizer and text encoder are used. Test
artifacts remain local and untracked. QtTest results are written explicitly;
the process's exit code alone is not accepted as a passing test.

```sh
PATH=/usr/bin:$PATH /usr/bin/python3 examples/semantic-search/tests/run \
  --corpus .tools/photo-search/icloud-pl-v1/corpus.json \
  --images .tools/photo-search/full-v2/clip-hybrid-ane \
  --rankings .tools/photo-search/full-v2/clip-hybrid-ane-rankings.json \
  --output .tools/photo-search/ui-integration-NEW
```

The default is an offscreen Qt window. For native testing use `--native`
and launch its script on hidden workspace 5 as required by `CLAUDE.md`.
The test checks lazy creation, public catalog loading, import, expected
Polish rankings, keyboard navigation, viewer return, latest-query wins,
repeated catalog cancellation, narrow geometry, pointer activation,
reopening and the translated quarantine error. It never touches real
settings, originals or clipboard contents.
The tagging check opens the editor with the keyboard, saves and removes a
suggestion, exercises undo and redo, and verifies the stored source/photo
entry after the window closes.
It also checks exact tag matching, switching back to model search and
retrieving saved tags after the worker stops.

On this M2 Pro, the complete 384-photo interaction test passed both offscreen
and on native Wayland on hidden workspace 5, including the 420-px panel,
mouse activation, viewer return, cancelled catalog refreshes and the blocked
indexing action. The independent Python script suite passed 97 tests with
the optional model/compiler environment at that stage; the later full suite
passed 102 checks including the offline review browser and CPU-affinity
measurement contract. These checks do not replace human
relevance labels or an Orca screen-reader session.
