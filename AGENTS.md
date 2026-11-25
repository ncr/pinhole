# Working on Pinhole

Read [CLAUDE.md](CLAUDE.md): work on a branch, open a pull request, and run test windows on a hidden workspace.
Build Rust with Cargo and the checked-in lockfiles.

Start with [README.md](README.md) and [docs/development.md](docs/development.md). The current architecture and measured
risks are in [the project review](docs/reviews/2026-11-03.md); the older
[core/plugin migration](docs/core-and-plugins.md) is implementation history.

For plugin work, read [the agent contract](docs/plugin-agents.md) and
[the API](docs/plugins.md). `pinhole plugin docs --json` gives the installed
API version and the same bundled documents without a window. New public
capabilities must update the contract, checker, examples and Polish strings.

`bin/test-all` runs every test below at once, none of them on the desktop,
in about a minute and a half; what each said is in `app/build/test-all/`.
Run it before landing. One by one:
use `app/bin/test [QtTest method …]`, `plugins/calendar/tests/run`,
`plugins/live/tests/run` and
`python3 -m unittest discover -s bin/tests` and
`python3 -m unittest discover -s examples/semantic-search/engine/tests`. Core tests run with
`cargo test --release --locked --workspace --no-fail-fast --manifest-path core/Cargo.toml`.
Record failures honestly. The independent JPEG regression is documented in
[the JPEG fixture notes](docs/reviews/jpeg-repro/README.md).
Run `app/bin/test-framestats`, `app/bin/test-queries` and
`app/bin/test-accessibility` for the dedicated contracts.
[ci/README.md](ci/README.md) describes the pinned container checks, including
separate Rust, visual and native Wayland jobs.

Run `app/bin/test-speed` after any change that could make something slower:
starting, a plugin's load, the library read again, a view opened or closed,
scrolling. It times them with a library and plugin data of years (20,000
photos, 20,000 faces named) against budgets and exits 1 over any: a cost that
grows with what a person has never shows with an empty profile, as People's
34-second start did not. Each window is on a display of its own. When it
fails, `--qml-profile` says which QML took the time (`app/bin/qml-hotspots`);
`PINHOLE_SNAPPY_ONLY` runs one step. A new view with a key is timed by itself.

Use `app/bin/measure-startup --help` for repeatable launch measurements.
Separate fresh QML caches from warm launches and report distributions,
first frame and photo readiness. Xvfb runs must explicitly use
`QT_QPA_PLATFORM=xcb`; inherited `wayland;xcb` can open a test on the real
desktop. Native tests belong on hidden workspace 5 (CLAUDE.md).
`app/bin/test-selftests`, `test-accessibility` and `test-places` get a display
of their own from `app/bin/headless-x`: Xvfb on a number from 100 up that no
other run has, or where it is not installed, a rootful Xwayland that
Hyprland starts on workspace 5 silently (`bin/test-all` and
`test-selftests` want Xvfb). Never use
the user's clipboard or Pictures for a self-test.

Use the shared `ShellButton`, `Toggle`, `FieldInput` and `Choices` controls.
Internal menus share `MenuCard`, `MenuRow` and `ControlBackground`; improve
these components rather than drawing another menu style. Pointer opening
must not focus an arbitrary action. Keep saved selection separate from
hover/keyboard position, and support opener toggle, outside click and Escape.
`app/bin/test-accessibility` covers these interactions, bounds and scrolling.
