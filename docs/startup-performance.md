# Startup performance

Measured on 2026-11-03 against baseline `d21d80d`, with the startup fixes
from the project review. Ryzen 7 9800X3D, NVIDIA RTX 5080, Qt 6.12.0,
49,092 real library items, all 16 built-in plugins enabled, copied plugin
settings/state/cache. Photos were only read. Each set has one warmup and
five measured launches; QML and graphics caches survive between launches.
The desktop remained in use, so ranges matter.

| Milestone | Before, median (range) | After, median (range) |
|---|---:|---:|
| Native Wayland first frame, 1055×1300 | 1007 ms (939–1402) | 416 ms (404–489) |
| Xvfb first frame, 1400×900 | 916 ms (895–976) | 463 ms (431–1044) |
| Xvfb all visible thumbnail cells resolved | 971 ms (949–1031) | 498 ms (468–1181) |
| Xvfb media integration available | 796 ms (773–855) | 885 ms (850–900) |

Native first frame is **59% sooner**. Media readiness is separate: moving
its initialization off the first-frame path does not make GPU probing
itself faster. “Resolved” means the initial thumbnail tier is on screen,
not that full originals have been downloaded or every tile has its final
zoomed-in rendition.

The private `pinholed --given` service was already ready for each UI run;
its separate boot-to-protocol-hello median was about 54 ms in Xvfb. This
does not measure a shared daemon restoring an interrupted storage move,
listing cloud sources, authenticating, or loading external providers.

## Cause and fix

`main.cpp` already initialized Qt Multimedia on a worker and delayed the
main photo viewer. However, the Photo frame plugin created its full
`Frame.qml` at launch. Its two `Slide.qml` objects created media players,
which waited for that same initialization on the GUI thread. One native
control run spent 551 ms inside loading `pinhole.frame`; after making the
view on demand it spent 3 ms. Its actions and settings still load at launch.

The calendar also built its whole-library histogram before anyone opened
it. Its histogram is now built when the calendar or its settings are first
opened. Grouping/hiding plugins still restore their cached grid state
before the first frame; deferring all plugins would flash ungrouped or
hidden photos and change the initial layout.

The new handbook is a URL-loaded `Loader`, inactive until requested. Its
Markdown is read only inside it or through the explicit CLI docs command.

## Reproduce

```sh
app/bin/build
app/bin/measure-startup --library /path/to/library --output /tmp/pinhole-startup \
  --app-cache ~/.cache/pinhole \
  --plugin-cache ~/.cache/pinhole/plugins \
  --plugin-state ~/.local/state/pinhole/plugins \
  --plugin-settings ~/.config/pinhole/plugins.json
```

The tool opens its own Xvfb display with an explicit `xcb` backend, uses
temporary XDG folders, copies only the requested caches/settings, starts
an owned `pinholed --given` with a private socket/cache, then cleans up its
processes. It emits logs and `results.json`. It does not sync or write the
library. `--no-plugins` is a diagnostic control; it is not the shipped UI.
`--warmup 0` can measure a fresh application cache, which is still **not a
cold OS boot or a cold filesystem**. Do not drop system-wide caches on the
user's working machine.

`--native-hidden` measures the current compositor; invoke it on hidden
workspace 5 as [CLAUDE.md](../CLAUDE.md) requires. A hidden surface may get
only its first frame. Native mode therefore waits for first frame and
media readiness, while `first_photos` can be absent; use Xvfb for a complete
and reproducible thumbnail-ready measurement. For the comparison above,
`--builtin-plugins` selected a temporary mirror containing the baseline
Frame and Calendar roots, then the current plugins, in sequential sets.
Both sets used the same instrumented executable.

`PINHOLE_STARTUP_TRACE=1` prints one compact JSON record for each milestone
and the synchronous load time of each plugin. Milestone times start at
entry to `main`; the dynamic loader and launcher precede them. The
measurement tool's `observed_ms` includes process launch and pipe delivery.
Parallel phases overlap: do not sum their milestone values.

Raw runs, including warmups, are in
[2026-11-03-startup.json](reviews/2026-11-03-startup.json). A fresh-QML-cache
exploration took roughly 1.2 s to resolve thumbnails before the fix; it is
not interchangeable with the warm-launch table above.

## Remaining budget

Aim next for a native warm first frame under 300 ms on this machine,
without changing the initial photo grouping or hiding. Profile the
remaining ~100 ms of plugin work, eager settings construction and the
first-render interval. Keep media readiness a separate metric. A five-run
sample is useful for regressions; it does not establish a production p95.

## Follow-up verification

After the compiler/query/frame/accessibility and module-boundary fixes, the
same real 49,092-item library and 16 plugins were measured again on native
Wayland, with a warmup and five launches on hidden workspace 5:

| Milestone | Median | Range |
|---|---:|---:|
| First frame | 391 ms | 387–409 ms |
| Visible thumbnail tier resolved, when reported | 473 ms | 461–503 ms |
| Media integration ready | 874 ms | 852–878 ms |

The first-frame improvement is preserved. This small sample does not prove
that the 26 ms difference from the prior 416 ms median is a durable gain.
As above, a hidden compositor surface can suspend uploads; the photo-ready
numbers were observed in this set, not guaranteed by native-hidden mode.
The window remained 1055×1300. The private read-only daemon boot is timed
separately in [the raw follow-up runs](reviews/2026-11-03-followup-startup.json).

The CI compositor check also ran successfully with a 10,000-item synthetic
library, private headless Weston and two CPU cores: first frame 369 ms
median, 60 Hz scroll at 5,000 px/s with p99 17.2 ms and no late frames in
that smoke phase. These software-renderer figures do not replace desktop
GPU measurements. Routine CI uses 2,048 synthetic photos to keep fixture
creation from dominating the job; its artifacts retain actual timings.

## Measuring the Qt host when the service cannot run

`--host-only` is an explicit diagnostic mode for a checkout whose service
cannot be rebuilt or does not speak the current command line. It leaves the
private socket unavailable and sets `PINHOLED` to `false`, so retries cannot
start either the checkout or installed daemon. It does not sync, fetch
renditions or write the library. Reports label `service_mode: unavailable`
and `service_boot_ms: null`. This mode is **not comparable to the warm-service
measurements above**, and never activates automatically after a failure.

For example, on a hidden native test workspace:

```sh
PATH=/usr/bin:$PATH app/bin/measure-startup \
  --library /path/to/library --output .tools/startup-host-only \
  --native-hidden --host-only --warmup 1 --runs 7
```

The M2 Pro checkout currently needs this mode: the old available pinholed
rejects `--given`, and rebuilding the pinned Blaze tool first fails on
16-KiB system pages. The same code can still build and exercise Qt directly.
Use the normal private warm-service mode again once a compatible daemon is
available. A synthetic-process regression verifies that host-only mode
launches no daemon and retains its explicit report label.

The On this day plugin now keeps its date index and button count available
at startup, while creating the larger `Days` view only on the first D.
That view is retained after closing, including the return from a photo.
The existing covering/return self-test and lazy-plugin scenario pass on an
isolated library; initial grouping, hidden photos and the day count stay
on the original first-frame path.
