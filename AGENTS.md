# Working on Pinhole

Read [CLAUDE.md](CLAUDE.md) for the delivery and hidden-workspace workflow.
Build Rust through `bin/build-rust` (pinned Blaze); Cargo is an explicit
reference check, never a silent retry after failure.

Start with [README.md](README.md). The current architecture and measured
risks are in [the project review](docs/reviews/2026-11-03.md); the older
[core/plugin migration](docs/core-and-plugins.md) is implementation history.

For plugin work, read [the agent contract](docs/plugin-agents.md) and
[the API](docs/plugins.md). `pinhole plugin docs --json` gives the installed
API version and the same bundled documents without a window. New public
capabilities must update the contract, checker, examples and Polish strings.

Use `app/bin/test [QtTest method …]`, `plugins/calendar/tests/run` and
`python3 -m unittest discover -s bin/tests`. Core tests run with
`bin/build-rust test --release --workspace --no-fail-fast --manifest-path core/Cargo.toml`;
use `BLAZE_TESTS=all` when auditing correctness, so cached results do not
hide a failure. Record failures honestly; the review includes a known JPEG
decoder parity failure and its standalone reproduction.

Use `app/bin/measure-startup --help` for repeatable launch measurements.
Separate fresh QML caches from warm launches and report distributions,
first frame and photo readiness. Xvfb runs must explicitly use
`QT_QPA_PLATFORM=xcb`; inherited `wayland;xcb` can open a test on the real
desktop. Native tests belong on hidden workspace 5 (CLAUDE.md). Never use
the user's clipboard or Pictures for a self-test.
