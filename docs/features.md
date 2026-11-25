# What Pinhole does

A fast photo browser for Omarchy: iCloud Photos on demand, or a folder of
photos on this computer. Working name.

Pinhole keeps an index of the whole library on disk (metadata and small
thumbnails) and fetches everything else from iCloud only when it is looked at,
dragged or exported, the way Photos on a Mac does with "Optimize Mac Storage".
It adds to the library: photos and videos dropped on the window go into it
(uploaded to iCloud, copied into a folder). It never deletes from it. It
edits a photo only when you ask: Turn and crop (R, K) on an iCloud photo
changes that photo in iCloud, so your iPhone shows it turned or cropped
too; Ctrl Z turns it back.

A folder works too: in the side panel (Tab), Add a source → a folder.
Pinhole indexes it and makes the thumbnails in the background, shows its
photos and videos as they are (an iPhone's `IMG_1234.HEIC` with
`IMG_1234.MOV` beside it is a Live Photo), and follows changes to it. How
sources fit in is in [docs/sources.md](sources.md).

Everything that is set is in that side panel: the sources, where Pinhole
keeps its files and how much room they take, and the plugins. iCloud,
Folders, Favourites, Turn and crop, Live Photos, Info and Screenshots are
on from the start; Send to phone, Series, People, the calendar, On this
day, Mosaic, Rainbow wall, Contact sheet, Photo frame, Background and
Theme are there too, off until turned on.

Small marks in a photo's corner say what it is: a video, a Live Photo, a
favourite, several photos as one card, and whatever a plugin marks; each
can be turned off in the side panel.

Series, the first plugin, finds photos of one subject taken close together
and shows each series as one card in the grid; under a photo of one, the
strip underlines the series. What it finds can be put right: on a photo,
X takes it out of its series, A adds it to the series beside it, U
ungroups the series; in select mode, G groups what is ticked. Ctrl Z
undoes any of it, as it does a favourite, and Ctrl ⇧ Z does it again. It
also (G in the grid, or on a photo of one) makes a series a short GIF or
video to send: Flip, Bounce, Fade, Drift or Pile, at a pace, in a look and a
shape, held steady; ⏎ copies it and keeps it in Pictures/Pinhole. It is
off until turned on in the side panel.

On this day shows the photos taken on today's date in earlier years, a year
at a time, the latest first: D in the grid, where its button says how many
years there are. In the side panel, the day alone or a few days around it,
and whether videos and screenshots count.

Contact sheet makes the photos ticked in select mode one picture to send
(K): an even grid, or a collage that keeps each photo whole, with a gap, a
background, a size and the dates if you like; ⏎ copies it and keeps it
in Pictures/Pinhole.

Send to phone (O, on a photo or what is ticked) sends photos to a phone or
computer nearby with LocalSend open: the devices show as they answer, and
once its owner accepts, the originals go, a Live Photo with its video, or
HEIC as JPEG (J). It is off until turned on in the side panel.

Info, on I on a photo, shows its details in a small panel over its corner:
when it was taken, the camera, lens and exposure, its size and format (a
video's codec, frame rate and length), and where. It stays open from photo
to photo. A video's original over 50 MB is read only when asked (⇧ I).

Map (M in the grid or in a located photo's Info panel) shows the current
library's photos and videos on a map Pinhole draws itself from
OpenFreeMap's vector tiles of OpenStreetMap: in the theme's colours, sharp
at any scale, with names in your language where the map has them and in
Latin letters elsewhere. Drag or slide
two fingers to move, and pinch or turn the wheel to zoom. Click or tap a
marker to open a photo or see its group, with the years it was taken in;
a photo opened there steps ← → through that place alone. Use **Zoom to
group** to look closer, or double click the map to zoom at the pointer.
**Photos in view** follows the visible area as you move.

With the keyboard, arrows choose a place or navigate its photo grid, Shift
with arrows moves the map, and Enter opens. Home fits all places, + and −
zoom, Z looks closer at the selected group, and G toggles **Photos in view**.
Escape goes back one view, and M closes the map.
GPS comes from iCloud or local metadata without fetching originals. Existing
libraries fill in their GPS data on the next catalog refresh. Pinhole sends
only the requested map tile coordinates to OpenFreeMap; the photo index
and thumbnails stay local. Cached tiles and photo markers work offline.

Mosaic makes any photo out of your other photos (H): each tile is a photo
of the colour the picture has there. A slider, + and −, or the wheel make
the tiles bigger and smaller, Space flips to the original and back, a
click on a tile opens its photo, and E exports the mosaic to a folder of
your choosing, by pixels, by paper and dots an inch, or by megabytes. A mosaic
shows large shapes that differ strongly, not small detail, so it looks
through the library for the photos that have them and lists the best under
the mosaic.

Rainbow wall lays as many of your most colourful photos as fill the window
out by hue (W), light at the top and dark at the foot. Space makes a wall
of other photos, + and − or a pinch make them bigger and smaller, and a
click on one opens it. It goes by each photo's average colour, so it is of
photos that are mostly one colour (a sunset, the sea, a forest) and not a
red coat on a grey street.

[Photos of screens](../plugins/screens/README.md) finds the photos taken of a
screen, a game on the TV, an error on a monitor, a machine's display, in
thumbnails on this computer, in pinholed. They are not screenshots: those
are the Screenshots plugin's. Shift+D shows them together; the side panel
can hide them from the grid, and so from the wall, the mosaic and the photo
frame. In select mode, Photo of a screen says the photos ticked are or are
not ones; the photos
very like it follow, on your other computers too.

Screenshots keeps screenshots from crowding out the photos: each day's one
card, or (⇧ S, or the side panel) not in the grid at all, with a button
saying how many are away. A screenshot is what iCloud marks as one (an
iPhone's or an iPad's), a file named as one (screenshot-…, Screenshot …,
Zrzut ekranu …, hyprshot's, grim's), or a PNG with no place shaped like a
phone's screen.

A plugin of your own is a folder in
`~/.config/pinhole/plugins` with a manifest.json and a QML file, loaded
again whenever you save it: [docs/plugins.md](plugins.md). It can
bring a source of photos of its own (Immich, a NAS, a phone) through a
provider, a program in any language that pinholed talks to in JSON lines:
[docs/source-providers.md](source-providers.md).

Settings (Tab) → Plugins → Build a plugin opens the offline plugin workshop:
a quick start, searchable API/reference sections, an Examples tab with
complete view/tool/source plugins, and instructions to copy for an agent. Tab/Shift Tab move through settings,
Escape closes them, and left/right change a focused section tab. In the
workshop, Ctrl Tab / Ctrl Shift Tab change documents and Ctrl F searches.
`pinhole plugin docs --json` gives the
installed API version and the same bundled documents to tools. Start with
[the quick start](plugin-quickstart.md) or [the agent contract](plugin-agents.md).

The [local photo-content search plugin](research/photo-search-plugin.md)
indexes thumbnails automatically in pinholed, even after the window closes. Ctrl F opens
search over the whole window: describe a photo in English or Polish and the
best matches come first. ⏎ opens the best one, and ← → go through the
answers alone; Esc comes back to them. A line under the question says how
much of the library can be found yet. The local
profiles use WeMM 4B on NVIDIA CUDA, CLIP on Apple Silicon with ANE,
or WeMM 4B on Apple GPUs with Asahi Vulkan when explicitly enabled.

[People](../plugins/people/README.md) groups similar faces in thumbnails
automatically in pinholed, even after the window closes. Shift+F shows the
people named so far and the faces not named yet. A group opens with every
name a key away (1–9), the likeliest first; once a few photos have a name,
groups that look like that person say so, and C on the person checks them
one after another. On a photo shown full window, a button says who is in it
(Anna, or Anna? for who a face looks like); W rings the faces and names each
one in turn: ⏎ for the likely one, a name typed, or Not Anna when it is wrong.
The names you give, and which faces are or are not someone, go to a Pinhole
folder in the iCloud Drive of the library's Apple ID, with the name of the
computer they were given on, so your other computers know them too. The
photos and faces themselves stay on the computer.

Background: B on a photo shown full window makes it the desktop background;
a video becomes a moving one, and on a Live Photo V its motion, as taken or
as its Loop, and ⇧ V its Bounce (both also under ▾, ⇧ B). In Series' studio,
B makes the video just made the background. Each is kept in
Pictures/Pinhole/.backgrounds, hidden from a folder source: the newest 12,
and any set now or a theme will bring back; a file put there otherwise is
never removed. Ctrl Z puts the one before back.

Year calendar (Y in the grid) shows every day of every year as a square in
the theme's accent, brighter the more photos it has, one row of weeks a
year; a click, or ↑ ↓ ← → and ⏎, shows that day in the grid.

Theme makes an Omarchy theme from the photo shown full window: T (or
Theme) sets it, the photo as the background and its colours on the whole
desktop, Pinhole too, the text always readable; Ctrl Z goes back to the
theme before. The last 12 made or used stay in Omarchy's theme picker; a
copy of one under another name is yours, and stays.

Photo frame (Frame, P, in the grid; P on a photo to start from it, or in
select mode for what is ticked) shows photos one after another, full
window, each slowly panned and zoomed and faded into the next: favourites,
all, or those taken on this day in other years, shuffled or in order,
videos and Live Photos moving. The screen stays on while it shows. Esc or
a click stops it, ← → step, and a two-finger tap or Space holds it still.

Zooming the grid sharpens visible tiles with the medium rendition, or the full
photo when the preview is missing or too small. Decode size follows the square
crop in device pixels, including portrait screenshots. Full-photo decodes run
two at a time once motion settles.

C copies the photo under the grid cursor, or the selected photos in select
mode. A small confirmation appears once the clipboard is ready; the Copy
button confirms selected photos in place. Ctrl C remains available too.

The main grid defaults to Automatic order: photos added in the last 30 days stay
at the newest end, ordered by addition date. Older additions sit where their
capture dates belong. The date chip's menu (or Ctrl Shift O) and the side
panel offer Automatic, Date taken and Date added; the last choice is kept
as the default. Capture dates, calendar and series data stay unchanged. The
viewer follows the grid's order too. A newly extended series shows its latest
addition on its card; archived series keep their original cover.

The main grid ends in a complete row; spare cells before its oldest photo
form a history card with the library's years, colours and a jump to the newest.

