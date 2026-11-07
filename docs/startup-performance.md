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

The M2 Pro measurements below used this mode because the available daemon
at the time rejected `--given`. Use normal private warm-service mode with
the current daemon built by `bin/build`. A synthetic-process regression verifies that host-only mode
launches no daemon and retains its explicit report label.

The On this day plugin now keeps its date index and button count available
at startup, while creating the larger `Days` view only on the first D.
That view is retained after closing, including the return from a photo.
The existing covering/return self-test and lazy-plugin scenario pass on an
isolated library; initial grouping, hidden photos and the day count stay
on the original first-frame path.

The selection bar is now constructed on the first selection and kept for
later use. Its original visual stacking position stays below plugin and
modal overlays. Settings keep their focus/dialog shell, but source, mark
and plugin rows and their sorting wait for the first opening. Closed action
menus no longer measure labels. Ctrl+A selects the grid through a registered
command, and Select all is also available through Range's menu.

A full-window accessibility check caught a narrow-layout bug: a long date
overlapped More and opened both the photo-order and action menus on one tap.
The date now fits before the action controls; folding uses its natural text
width to avoid a layout cycle. The isolated check covers this geometry and
the two pointer clicks, plus first-use settings/selection construction.

## M2 Pro follow-up: CPU placement

With 49,095 local items, all 16 built-in plugins, warm caches, native
Wayland on hidden workspace 5 and an 854×1062 window, the first-frame
milestones were:

| Condition | Measured runs | Median | Range |
|---|---:|---:|---:|
| Before the lazy UI changes, inherited CPU affinity | 7 | 395 ms | 353–494 ms |
| Lazy views, selection and settings, inherited affinity | 10 | 347 ms | 303–426 ms |
| Same checkout, Qt host on CPUs 4–11 | 10 | 285 ms | 262–347 ms |

These are **host-only diagnostics with no photo service**. The final two
sets ran without concurrent builds/tests; the earlier exploratory set
with concurrent UI testing is excluded. Each final set used two warmups.
The small, unpaired distributions overlap and do not establish production
p95 or an isolated causal gain from each individual lazy component. They
are not comparable to the Ryzen warm-service 391-ms result above. Native
hidden mode's media-ready event is not a resolved-photo measurement.

On this M2 Pro CPUs 0–3 report capacity 561 and CPUs 4–11 capacity 1024.
Restricting the entire diagnostic launcher to 4–11 first yielded 284 ms
median (271–317 ms); restricting only the Qt child through the explicit
tool option reproduced 285 ms. This implicates CPU placement, but does not
justify pinning the production app without scroll, decode and power checks.
**The default app has no affinity change; the general under-300-ms goal
remains open.** Sanitized runs and plugin/milestone timings are in
[the M2 Pro measurement artifact](reviews/2026-11-03-mac-startup.json).

The diagnostic option is Linux-only, validates currently available CPUs
and records both requested and effective host affinity. It does not pin
the private service or change system settings. CPU numbers are specific to
this machine; inspect the target hardware before choosing them.

```sh
PATH=/usr/bin:$PATH app/bin/measure-startup \
  --library /path/to/library --output .tools/startup-pcores \
  --native-hidden --host-only --warmup 2 --runs 10 --cpu-affinity 4-11
```

## M2 Pro with a warm private service

On November 4 the Cargo-built `pinholed` served the 49,095-item local
library in read-only mode. Both sets used the same 854×1062 Wayland window
on hidden workspace 5 and inherited CPU affinity. The grid now filters the
timeline's existing presentation order directly, skipping a second sort
when building visible cells. Stacks and hidden photos are applied before
the first frame.

| First frame | Runs per build | Before, median (range) | After, median (range) |
|---|---:|---:|---:|
| Warm QML and graphics caches | 10 after 2 warmups | 349 ms (280–463) | 352 ms (330–436) |
| Fresh app caches on a warm filesystem | 3 isolated launches | 655 ms (620–748) | 570 ms (490–645) |

The warm runs show no measurable first-frame gain and the median still
exceeds 300 ms. The fresh-cache sample is small and the ranges overlap.
Native hidden windows reported the first frame and media readiness but
no resolved-photo milestone in either set.

## Avoiding work before the first frame

The automatic timeline now keeps older photos in the index's capture
order and sorts the recently added partition by addition date. It checks
capture order after converting timestamps, so a missing date in an
older index still sorts correctly relative to dates before 1970. The
timeline step fell from about 65 ms to under 1 ms in the isolated
same-library comparison.

The plugin host walks source trees once for both change stamps and file
watches, pruning helper build directories before traversal. Help, the
source pickers, tours, and the Series editor and studio are built on
their first use. The Series editor and studio contribute their own
commands to Help once instantiated. Grouping and hiding still apply on
the initial grid. The qmake build compiles the application's bundled
QML with the [Qt Quick Compiler](https://doc.qt.io/qt-6.12/qmldiskcache.html).
Plugin source files still load from disk and can be edited and reloaded.

The following November 4 runs used the same 49,095-item library, all
16 built-in plugins, the warm read-only private service, and the
854×1062 Wayland window on hidden workspace 5. The app and service
inherited the desktop's affinity. Warm sets had two warmups each.

| First frame | Before, median (range) | After, median (range) |
|---|---:|---:|
| Warm caches, from entry to `main` | 395 ms (326–461), 20 runs | 291 ms (245–318), 15 runs |
| Warm caches, from process launch | 430 ms (376–516) | 332 ms (289–360) |
| Fresh app caches, from entry to `main` | 528 ms (520–597), 3 runs | 332 ms (320–381), 3 runs |
| Fresh app caches, from process launch | 574 ms (567–645) | 365 ms (360–422) |

Twelve of the fifteen warm launches reached a first frame within 300 ms
of entering `main`. Including the dynamic loader and trace delivery, the
median remains above 300 ms. The fresh sets used a warm filesystem and
new QML and graphics caches for each launch. Native hidden runs did not
report photo readiness. A separate Xvfb/OpenGL run on a 384-item fixture
reported a median of 437 ms to resolve the initial thumbnail tier from
`main` (423–465 ms over five warm launches).

## Speed at scale (2026-11-07)

People's names of 9,627 faces kept Pinhole from showing anything for 34
seconds: `pinhole.synced` made each key a property one at a time, and Qt
makes a property map's type again for every new key. Nothing caught it: the
startup measurements and CI used an empty profile. `app/bin/test-speed` now
gives every check a library and plugin data of years (20,000 photos and
20,000 faces named, or a real library and synced folder read only) and fails
over a budget:

- starting (`measure-startup`, which now copies a synced folder and fails
  over `--max-first-frame-ms`, `--max-plugin-ms`, `--max-plugins-ms`);
- the window standing still (`PINHOLE_SELFTEST=snappy`: a timer on the
  window's thread, the longest it waited) as the library is read again five
  times, a photo opens and closes, and every plugin view with a key opens
  and closes, after a first run that fills the QML cache as use does;
- scrolling at 5,000 px/s, reported.

CI's native job runs it on headless Weston with budgets for two cores
drawing in software. Its first run here found, besides the start:

| What | Before | After |
|---|---:|---:|
| First frame, real profile (fixer / spawner) | 34,033 / – ms | 482 / 335 ms |
| People's load, 20,000 faces | 33,744 ms (9,627) | 90 ms |
| Library read again: window still | 106–137 ms | 13–49 ms |
| Any view opened or closed: key clashes looked for | 20–90 ms | 2–12 ms |
| People opened (Shift+F) | 236–265 ms | ~120 ms |
| A photo shown with People on (faces asked for) | +87 ms | once per change |
| Calendar opened / closed (Y) | 192 / 198 ms | 54–73 / 13–22 ms |

The causes: the key clash check asked every Command for every key of every
action (now an index made once); `pinhole.synced.values()` was a QVariantMap,
whose every key read in JS goes back to C++ (now a plain object); People
went through all its faces' names and wrote them all as JSON each time a
view opened or a photo was shown (now once for each change); the calendar
drew each year's glowing squares on the window's thread in every frame of
its opening (now `Canvas.Threaded`).
