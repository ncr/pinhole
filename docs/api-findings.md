# iCloud Photos web API: what the probe found

Measured on 2026-10-24 against a real library (48,948 items, 2001–2026) from
an M2 Pro on home broadband, with `pinhole-probe`. Everything here is
read-only.

## Sign-in

- The pyicloud provider adopted an existing icloudpd session (copied, not
  shared) and Apple validated it without a password or a new code.
- A session is 22 cookies, the `ckdatabasews` URL and four query params.
  That is the whole hand-over; nothing else from pyicloud is needed.
- Download URLs (`cvws.icloud-content.com`) need **no cookies**: they are
  signed. Only CloudKit requests carry the session.

## Reading the library

| What | Result |
|---|---|
| Page size | **100 assets** (200 records) per list request, whatever is asked for |
| Request time | ~2.4 s per page, ~9.5 KB of JSON per asset |
| Full walk, 4 requests in flight | **5 min**, 161 assets/s, 490 requests, 0 retries, no throttling |
| Random access | Pages are addressed by position (`startRank`), so any part of the library can be fetched directly and pages fetched in parallel |
| Lookup by ID | `records/lookup`: 100 assets in ~2.3 s, 1–4 assets in ~0.4 s |
| Counts | `HyperionIndexCountLookup` gives totals instantly: library, favorites (608), videos (2,823), Live Photos (13,895), hidden (64) |

The index without raw records and without stored URLs is roughly **90 MB**
(487 MB with them; raw JSON is 298 MB, URLs 98 MB).

**Assets, not masters, are the unit.** A duplicated photo is a second
CPLAsset on the same CPLMaster: this library has 48,950 assets on 48,800
masters. Keyed by master, 150 assets went missing. A page is capped at 200
*records*, so with shared masters it holds 100–104 assets; since masters
never outnumber assets, a full page always holds at least 100, and stepping
pages by 100 positions can overlap but never skip. Keyed by asset record,
the index matches iCloud's count exactly. Halves split across pages turned
out not to happen, but `sync::walk` pairs them up (looking up missing
masters) in case they do.

## Staying up to date

- `records/query` returns a `syncToken` with each page.
- `changes/zone` with the token from the start of the walk answered in 345 ms:
  0 records, `moreComing: false`, and a new token. The incremental feed
  works mechanically.
- **Not verified yet:** that a new photo, a deletion and a favorite change
  actually show up in it. Needs a real change on the phone.
- Reading the feed from scratch is no use for the first sync: 200 records per
  page, masters first and assets later, and pages must be read one after
  another.

So: first sync by parallel list pages, then the change feed from the token
saved before the walk.

## Renditions

From the metadata alone, for the whole library:

| Rendition | Items | Total | Average | Long edge (median) |
|---|---|---|---|---|
| JPEGThumb | 48,797 | **3.1 GB** | 64 KB | **480 px** |
| JPEGMed | 39,051 | 30 GB | 769 KB | 2048 px |
| Original | 48,798 | 258 GB | 5.3 MB | 3264 px |
| OriginalVidCompl (Live Photo video) | 13,875 | 47 GB | 3.4 MB | 1744 px |
| VidMed | 16,666 | 60 GB | 3.6 MB | 720p |
| VidSmall | 16,659 | 19 GB | 1.1 MB | 581 px |

Edited photos carry their renditions on the CPLAsset record and the original
on the CPLMaster; the asset's copy is the one to show.

Originals: 26k JPEG, 19k HEIC, 2.7k QuickTime, 850 PNG, a few MPEG-4, AVI,
GIF and TIFF.

## Downloading

| What | Result |
|---|---|
| Thumbnail latency | ~470 ms to first byte, regardless of concurrency |
| Thumbnails, 64 in flight | **115/s** (7.4 MB/s); the whole library in ~7 min if it holds up |
| Medium JPEGs, 32 in flight | 43/s, 32 MB/s |
| URL lifetime | still valid after 5 min, **410 Gone after 35 min** |

Throughput scales linearly with requests in flight; the limit is latency, not
bandwidth. Not yet tried: sustaining it for all 48k thumbnails.

## Video

- Every video rendition answers ranged requests with `206 Partial Content`
  and `Accept-Ranges: bytes`.
- First byte 0.8–2.3 s for a cold file; a seek inside a file already opened
  ~25 ms (sometimes 0.4–1.4 s).
- Reads at 40–60 MB/s (twice 6 MB/s) against bitrates of 3–4 Mbit/s for
  VidMed and 11–18 Mbit/s for originals: streaming with seeking works.
- Content-Type is always `application/octet-stream`; the player needs the
  file type from the metadata.
- Durations are in milliseconds.

## Live Photo effects

Measured on 2026-10-28 on the same library: every field of all 13,969
Live Photos, through `pinhole-probe record` (a lookup without
`desiredKeys`).

- **The effect is an edit.** Loop, Bounce and Long Exposure are an
  `AutoLoop` adjustment in `adjustmentSimpleDataEnc` (raw deflate, then
  JSON: `{"identifier": "AutoLoop", "settings": {"flavor": "AutoLoop" |
  "Mirror" | "LongExposure", "recipe": {...}}}`, the recipe holding the
  loop's period and fade and per-frame stabilisation). The same is in
  `adjustmentRenderType` as flags: **1 loop, 2 portrait, 4 long exposure,
  8 bounce** ("mirror"). icloud.com reads only the flags, and so does
  Pinhole. The two agreed on every photo: 57 loops, 47 bounces and 17 long
  exposures; one loop had no simple data, only the flag.
- **Apple keeps the renders.** An edited Live Photo's asset record has
  `resVidComplRes` (with Width, Height, FileType), the motion as edited:
  for a loop the crossfaded loop, for a bounce the clip forward and back
  (60 frames for 31 shot, stabilised and cropped), for a long exposure the
  stabilised motion. Smaller ones are its `resVidMedRes` and
  `resVidSmallRes`. A long exposure's still is the edit itself,
  `resJPEGFullRes` (HEIC, 1186x1582 for a 3024x4032 original). The
  original motion stays on the master, `resOriginalVidComplRes`.
- `viewPresentationTypeEnc` was 0 on every photo; `vidComplVisibilityState`
  takes several values (0, 5, 8, 10, 11, 13, 15) and is not the effect.
- **Changing the effect is not something the web does.** icloud.com
  (photos3, build 2640Build32) shows the effect as a badge (LIVE, LOOP,
  BOUNCE, LONG EXPOSURE) and plays a bounce by stepping its video back and
  forth itself; its code has no editor, writes no adjustment field, and
  the only `records/modify` it sends are favourites, deletions, hiding,
  dates, places, captions and albums.

Setting an effect from Pinhole would mean a `records/modify` of the
CPLAsset with a new `adjustmentType`, `adjustmentSimpleDataEnc` (an
`AutoLoop` recipe with Apple's stabilisation, which only Photos computes),
`adjustmentRenderType`, the timestamps and fingerprints, and new renders
uploaded for `resVidCompl`, `resJPEGFull`, `resJPEGMed` and `resJPEGThumb`
(CloudKit's asset upload, which icloud.com uses only for new photos). A
recipe the iPhone cannot read, or renders that disagree with it, would
show differently on every device, and nothing on the web can undo an edit.

## Location

`locationEnc` is a base64 binary plist with `lat`, `lon`, `alt`, `course`,
`speed`, accuracy and a timestamp: readable, so a map and place filters are
possible later. There are no place names; those would need local reverse
geocoding. 29,426 assets have a location.

## What this means for Pinhole

1. **Download every thumbnail up front.** 3.1 GB and minutes of transfer
   buys a grid that never waits on the network, at 480 px, which covers cells
   up to ~240 pt on a 2× display.
2. **Index sync:** parallel list pages for the first run (~5 min), then the
   change feed. Store record names, never download URLs.
3. **On demand:** look up fresh URLs when something is opened (≈0.4 s), then
   fetch the medium JPEG (≈0.5 s) and the original behind it.
4. **Video:** stream through a local ranged proxy that writes what it reads
   into the cache.
