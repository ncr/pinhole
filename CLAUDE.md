# CLAUDE.md

How to work on Pinhole as an agent. Start with [AGENTS.md](AGENTS.md); it lists the tests and conventions. The
author keeps his own delivery steps (landing, installing on his computers) in an untracked `CLAUDE.local.md`.

## When a change is done

Run `bin/test-all` and the dedicated checks AGENTS.md names for what you touched. Work on a branch and open a pull
request; do not push to `main`. To try a change in the app, `bin/install` builds the checkout and links this
account's launcher to it (`--uninstall` removes only the installed code, keeping photos, settings and the
repository).

## Windows a test opens go to workspace 5

A self-test, the bench or any other run of `app/build/pinhole` opens a real window, and the person at the computer is
working on the workspace they are on: start it on Hyprland's workspace 5, silently, never where they are. Put the run in a script (its
environment, output and exit code into a log) and hand that to Hyprland, whose config is Lua, so the old
`[workspace 5 silent]` syntax of `hyprctl dispatch exec` fails:

```sh
hyprctl dispatch "hl.dsp.exec_cmd('sh /path/to/run.sh', { workspace = '5 silent' })"
```

It returns at once: wait for the script's exit line in its log. The window there has the whole workspace, about 3200
wide, so a cell is bigger at a given number of columns than in his own window beside others.
