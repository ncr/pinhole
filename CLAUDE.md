# CLAUDE.md

## After every feature

Jacek develops Pinhole and tries each change in the app he opens with Super+Space. When a feature or fix is done,
without asking:

1. Commit it and land it on `main` (a fast-forward push; this repository has no merge queue).
2. Install it on this computer with `bin/install-local`: it builds the checkout into `~/.local`, needs no root
   password and no package, and stops the running window and its pinholed so the next launch is the new version.

Then say what changed in the app. `bin/install` (the pacman package, which asks for the root password) is for other
people's machines, not for this loop.
