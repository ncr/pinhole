# CLAUDE.md

## After every feature

Jacek develops Pinhole and tries each change in the app he opens with Super+Space. When a feature or fix is done,
without asking:

1. Commit it and land it on `main` (a fast-forward push; this repository has no merge queue).
2. Install it on this computer with `bin/install --non-interactive`: it builds the checkout and links
   the launcher directly to the repository, needs no root password and no package, and restarts any running
   windows and their pinholed after successful installation. A closed app stays closed.

Then say what changed in the app. `bin/install` is the single public source installer (`--dev` is a compatibility alias),
`--uninstall` to remove only this account's installed code while keeping
photos, settings and the repository. Package updates and removal belong to the OS package manager;
in-app updates apply only to developer installations.

## Two computers: fixer and spawner

Pinhole is made for both of Jacek's computers, and every change is installed and tried on both:

- **fixer**: its checkout is `~/Work/pinhole`.
- **spawner**: its checkout is `~/dev/pinhole`.
- A session runs on either (`hostname` says which) and reaches the other with `ssh fixer` or `ssh spawner`.
- They are on one tailnet and almost always on.

They show one iCloud library, and what plugins keep of it (`pinhole.synced`, People's names) goes between them
through iCloud Drive. So:

- **Think of both.** Whatever a change syncs, caches or keeps per computer must work when the other has its own
  of it, older or newer, and when the other is on an older Pinhole.
- **Land, then install on both.** After landing on `main`, install here as above, then on the other one, e.g.
  `ssh spawner 'bash -lc "cd ~/dev/pinhole && git pull --ff-only && bin/install --non-interactive"'`
  (a login shell: without it there is no `cargo` on fixer, and no `OMARCHY_PATH` for `omarchy` commands).
- **Check it on both.** For sync, look in each one's `~/.local/state/pinhole/synced/` and `pinholed.log`.
- **Test on spawner too**, when a change can behave differently there (its library, its state, its hardware).
  Its windows go to workspace 5 there as well. For `hyprctl` over ssh, set `HYPRLAND_INSTANCE_SIGNATURE`
  to the folder in `/run/user/1000/hypr`.

You may update, install, open and test Pinhole on spawner without asking.

Omarchy 4.5 can share a screen over ssh: use it when a window on spawner has to be seen.

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
