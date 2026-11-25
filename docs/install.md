# Installing Pinhole

Pinhole is for Arch Linux, and made for and tried on Omarchy. These parts
need Omarchy itself: the bar widget, Background and Theme (they set
Omarchy's background and theme), and opening Pinhole where the bar's icon
is dropped (Hyprland). Elsewhere the window, its sources and the other
plugins are there, in Pinhole's own colours rather than the theme's.

There is no package yet: Pinhole installs from this repository, in one
command:

```bash
git clone https://github.com/ncr/pinhole && pinhole/bin/install
```

`bin/install` installs what Pinhole needs and builds with through pacman
first (sudo asks for your password once), then builds it, which takes
several minutes the first time, and links it into `~/.local` for your
account. On Omarchy everything else is there already; a bare Arch install
needs `git` and `sudo` before it. Run it as yourself, not with
`sudo`. `gum`, if installed, gives it a small menu and a spinner.

```bash
bin/install                  # terminal UI; build and run directly from this checkout
bin/install --non-interactive # the same source install without the UI
bin/install --uninstall      # remove this account's install; keep photos/settings/repo
```

Then open Pinhole from the launcher and add a source in the side panel
(Tab): an Apple ID (password and two-factor code, or the session icloudpd
already has) or a folder. The library builds in the background, the index
in about 5 minutes and every thumbnail in about 6 more for 50,000 photos,
and the grid fills as it does. `?` lists every shortcut, and finds any
command by a few letters as you type: ⏎ does it.

The same from a terminal, without the window:

```bash
pinhole sign-in              # Apple ID, then the password and code, or icloudpd's session (iCloud's plugin on)
pinhole status               # the source shown, its library, how far it is built
pinhole help                 # every command: sources, show, remove, sync, add, storage…
```

Each asks the Pinhole service, `pinholed`, and starts it if it is not
running; only the service writes the library.

On Omarchy, `bin/install` also gives the bar a Pinhole widget: its camera
obscura lights up with the number of photos that came since you last
looked, and a click shows the newest as a small grid to drag them from
into a chat, a browser or a folder. Click photos to drag several at once.
Photos dropped on the icon are added to the source shown, as on the window;
a right click opens Pinhole. Dragged off the bar with the right button, the
icon opens Pinhole where you let it go, in the tile under the pointer as
Hyprland places a new window; an open Pinhole moves there.
It goes on the bar with `omarchy bar put io.github.ncr.pinhole`. It reads
what `pinhole recent --follow` says, drags what `pinhole export` makes (a
HEIC as a JPEG, as a drag out of the window does) and adds with
`pinhole add --json`, whatever the source.

`bin/install` builds the checkout and links the launcher, service and plugins to
it in `~/.local`, without a package or root password. QML plugins stay live in
the repository. After a successful install, running windows and their service
are stopped, then those windows reopen with their original arguments and
session environment. If Pinhole was closed, it stays closed. A failed build
leaves running processes alone. `PREFIX` changes the installation folder;
`--non-interactive` bypasses the menu for scripts. `--dev` remains an alias
for existing developer update helpers.

`pkgbuild/` holds a recipe for an Arch package, `pinhole-git`, not
published anywhere yet, and its `.SRCINFO`; after changing the
recipe, write that again with `makepkg --printsrcinfo > .SRCINFO`.
`bin/install` does not build or install packages. Both install
`share/licenses/pinhole/`: Pinhole's license and those of the Rust crates
its programs link, which `bin/build` writes (`bin/third-party-notices`).

Developer installations quietly check `main` hourly. A small Update button, also in the
side panel under Pinhole updates, builds the new version while you browse,
apart from your checkout, which moves to it only once all of it is built;
choose Restart when it is ready. Package updates and removal use the OS
package manager. Developer updates require a clean `main`
checkout and only fast-forward it, preserving local work. A checkout
without Git or an `origin` (a downloaded source archive) installs too, with
no Update button.

`--uninstall` stops this installation's Pinhole and its service (and with
it the plugins' work in the background, People's and the search's), then
removes only files recorded by the local/developer installer;
changed or replaced files are preserved, as are your photos, preferences,
custom plugins and source checkout. It never removes a system package. A
source's saved sign-in (an Apple ID's session) stays too: to forget it,
run `pinhole sign-out <source>` first, a source as `pinhole sources` lists
it.

