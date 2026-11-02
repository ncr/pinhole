# CLAUDE.md

## After every feature

Jacek develops Pinhole and tries each change in the app he opens with Super+Space. When a feature or fix is done,
without asking:

1. Commit it and land it on `main` (a fast-forward push; this repository has no merge queue).
2. Install it on this computer with `bin/install --dev --non-interactive`: it builds the checkout and links
   the launcher directly to the repository, needs no root password and no package, and stops the running
   window and its pinholed so the next launch is the new version.

Then say what changed in the app. `bin/install` is the single public installer: `--dev` for this loop,
`--package` for a pacman package, `--uninstall` to remove only this account's installed code while keeping
photos, settings and the repository. Package updates and removal belong to the OS package manager;
in-app updates apply only to developer installations.

## Windows a test opens go to workspace 5

A self-test, the bench or any other run of `app/build/pinhole` opens a real window, and Jacek is working on the
workspace he is on: start it on Hyprland's workspace 5, silently, never where he is. Put the run in a script (its
environment, output and exit code into a log) and hand that to Hyprland, whose config is Lua, so the old
`[workspace 5 silent]` syntax of `hyprctl dispatch exec` fails:

```sh
hyprctl dispatch "hl.dsp.exec_cmd('sh /path/to/run.sh', { workspace = '5 silent' })"
```

It returns at once: wait for the script's exit line in its log. The window there has the whole workspace, about 3200
wide, so a cell is bigger at a given number of columns than in his own window beside others.
