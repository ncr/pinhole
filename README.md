<img src="docs/readme/obscura.svg" alt="Pinhole: a fast photo browser for Omarchy. iCloud Photos and folders today, more sources coming. Its rainbow wall, seen through a camera obscura." width="100%">

<img src="docs/readme/keys.svg" alt="Pinhole's keyboard: W rainbow wall, R turn, T theme, Y calendar, I info, O send, P frame, S select, D on this day, F favourite, G series, H mosaic, K crop, L live, C copy, B background, N newest, M map" width="100%">

See your iCloud Photos on Omarchy: the whole library in one fast grid, with your favourites, Live Photos, people
and places, and every view a key away. A folder of photos works too. Photos kept somewhere else (Google Photos,
Immich, a NAS) are not there yet: see [Sources](#sources).

## Install

```bash
git clone https://github.com/ncr/pinhole && pinhole/bin/install
```

For Arch Linux, made for and tried on Omarchy. Installing takes several minutes the first time. Then open
Pinhole, press <kbd>Tab</kbd> and add your Apple ID or a folder. A big library takes a few minutes to appear, and
the grid fills as it does. <kbd>?</kbd> lists every shortcut.

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
<tr><td><b>Rainbow wall</b></td><td><kbd>W</kbd></td><td>Your most colourful photos, laid out by hue.</td></tr>
<tr><td><b>Mosaic</b></td><td><kbd>H</kbd></td><td>Any photo, made out of your other photos.</td></tr>
<tr><th colspan="3" align="left">↓ Imagined it, so I built it</th></tr>
</table>

Each one in full is in [What Pinhole does](docs/features.md).

## Not here yet

Pinhole is young, and some basics are not built: deleting a photo, albums, the iCloud shared library, and any
edit beyond turn and crop. For those you still need the app your photos come from. So nothing in Pinhole can
remove a photo from your library.

## Sources

iCloud Photos and folders work today. Google Photos, Immich, a NAS or a phone can each be added as a plugin, and
nobody has made one yet. If that is where your photos are, a pull request is very welcome:
[the prompt below](#make-your-own) builds one.

## Make your own

A new view, a tool or a source of photos is a plugin, and an AI coding agent can build one. Paste this into yours
(Claude Code, Codex, OpenCode…):

```text
Build a Pinhole plugin for me. If I want to share it, open a pull request against
github.com/ncr/pinhole with it.
0. Ask me first what I want, with questions such as: is it a new view of my photos,
   a tool that makes something from them, or a new source of photos (Immich, a NAS,
   a phone, another cloud)? What should it do, and what is it called? For a source
   also: its address and how you sign in there. Then pick a short id for the plugin
   yourself.
1. If Pinhole is not installed yet:
   git clone https://github.com/ncr/pinhole && pinhole/bin/install --non-interactive
2. Read `pinhole plugin docs plugin-agents` and follow it. It is the contract that
   Pinhole's own "Copy agent instructions" button gives (Tab, Plugins, Build a
   plugin), for the version installed, and it names the other documents and the
   complete examples to start from.
3. Begin the plugin with `pinhole plugin new`, and run `pinhole plugin check` on it
   until it passes.
4. Ask me to turn the plugin on (Tab, Plugins), and try it on my real library. Only
   read from my photos while you test; change and delete nothing where they are kept.
5. To share it: fork the repository, put the plugin's folder in `plugins/`, name it
   in README.md, run `bin/test-all`, and open the pull request. Say in it what you
   tried, on what, and what you could not try. Sign it with what you are: the agent
   and the exact model and version that wrote the plugin (and the reasoning effort,
   if you know it).
```

Something that does not work is worth a pull request too: it saves the next person the afternoon.

## More

- [Sources of your own](docs/source-providers.md), and [how sources fit in](docs/sources.md)
- [Plugins](docs/plugins.md) and [the contract for agents](docs/plugin-agents.md)
- [Developing Pinhole](docs/development.md): building, the layout, tests, the probe
