<img src="docs/readme/keys.svg" alt="Pinhole's keyboard: R turn, T theme, Y calendar, I info, O send, P frame, S select, D on this day, F favourite, G series, H mosaic, K crop, L live, C copy, B background, N newest, M map" width="100%">

A fast photo browser for Omarchy: iCloud Photos on demand, or a folder of photos on this computer. Working name.

Pinhole keeps a small index of the whole library on disk and fetches the rest from iCloud only when you look at
it, drag it or export it. It adds the photos you drop on its window.

## Install

```bash
git clone https://github.com/ncr/pinhole && pinhole/bin/install
```

For Arch Linux, made for and tried on Omarchy. The first build takes several minutes. Then open Pinhole, press
<kbd>Tab</kbd> and add an Apple ID or a folder. For 50,000 photos the index builds in about 5 minutes and every
thumbnail in about 6 more, and the grid fills as it does. <kbd>?</kbd> lists every shortcut.

More in [Installing Pinhole](docs/install.md): what the installer does, the bar widget, updates, uninstalling.

## Plugins

Sorted by one metric. The ones I use every day come first. Further down are the ones that exist because I could
imagine them. With AI an idea is enough to start building, and that is pure joy.

<table>
<tr><th colspan="3" align="left">↑ Useful every day</th></tr>
<tr><td><b>Favourites</b></td><td><kbd>F</kbd></td><td>A heart on a photo, in iCloud too.</td></tr>
<tr><td><b>Turn and crop</b></td><td><kbd>R</kbd> <kbd>K</kbd></td><td>Turns or crops the photo, on your iPhone too. <kbd>Ctrl Z</kbd> turns it back.</td></tr>
<tr><td><b>Info</b></td><td><kbd>I</kbd></td><td>Camera, lens, exposure and place.</td></tr>
<tr><td><b>Send to phone</b></td><td><kbd>O</kbd></td><td>Photos to a phone nearby, through LocalSend.</td></tr>
<tr><td><b>Screenshots</b></td><td><kbd>⇧ S</kbd></td><td>Keeps screenshots from crowding out the photos.</td></tr>
<tr><td><b>Search</b></td><td><kbd>Ctrl F</kbd></td><td>Describe a photo in English or Polish. It runs on your computer.</td></tr>
<tr><td><b>People</b></td><td><kbd>⇧ F</kbd></td><td>Groups similar faces; you give the names.</td></tr>
<tr><td><b>Live Photos</b></td><td><kbd>L</kbd></td><td>Plays the motion: Live, Loop, Bounce or Long exposure.</td></tr>
<tr><td><b>Map</b></td><td><kbd>M</kbd></td><td>Your photos on a map drawn in your theme's colours.</td></tr>
<tr><td><b>On this day</b></td><td><kbd>D</kbd></td><td>Today's date in earlier years.</td></tr>
<tr><td><b>Series</b></td><td><kbd>G</kbd></td><td>Photos of one subject as one card, or a short GIF.</td></tr>
<tr><td><b>Contact sheet</b></td><td><kbd>K</kbd></td><td>The ticked photos as one picture to send.</td></tr>
<tr><td><b>Year calendar</b></td><td><kbd>Y</kbd></td><td>Every day of every year as a square.</td></tr>
<tr><td><b>Photo frame</b></td><td><kbd>P</kbd></td><td>Photos one after another, slowly panned and zoomed.</td></tr>
<tr><td><b>Background</b></td><td><kbd>B</kbd></td><td>A photo or a video as the desktop background.</td></tr>
<tr><td><b>Theme</b></td><td><kbd>T</kbd></td><td>An Omarchy theme from the photo on screen.</td></tr>
<tr><td><b>Mosaic</b></td><td><kbd>H</kbd></td><td>Any photo, made out of your other photos.</td></tr>
<tr><th colspan="3" align="left">↓ Imagined it, so I built it</th></tr>
</table>

Each one in full is in [What Pinhole does](docs/features.md). A plugin of your own is a folder with a
manifest.json and a QML file: start with [the quick start](docs/plugin-quickstart.md).

## Not here yet

Pinhole is young, and some basics are not built: deleting a photo, albums, the iCloud shared library, and any
edit beyond turn and crop. For those you still need Apple's Photos. So nothing in Pinhole can remove a photo from
your library.

## More

- [Sources](docs/sources.md): iCloud, folders, and [sources of your own](docs/source-providers.md)
- [Plugins](docs/plugins.md) and [the contract for agents](docs/plugin-agents.md)
- [Developing Pinhole](docs/development.md): building, the layout, tests, the probe
