# Photos of screens

Photos taken **of** a screen: a game on the TV, an error on a monitor, a
laptop, a phone held up to the camera, a 3D printer's or a machine's
display. Not screenshots: what the library knows to be one (iCloud's mark,
a screenshot's name) belongs to the Screenshots plugin and is never one of
these.

Off until turned on in the side panel. Then pinholed runs
`pinhole-screens` for the library shown, even with the window closed: each
still thumbnail goes once through MobileNetV2 on the processor (two
threads, lower priority, about ten minutes for 46,000 photos), and a small
head (`head.json`) says from what it makes of it whether the photo is of a
screen. It needs Python 3 with OpenCV and NumPy, and fetches the model
once (14 MB, checked against its SHA-256) from
[OpenCV Zoo](https://github.com/opencv/opencv_zoo/tree/48b10fa505aeb51fbd127fc60a5c660040e823b7/models/image_classification_mobilenet),
[Apache 2.0](https://github.com/opencv/opencv_zoo/blob/48b10fa505aeb51fbd127fc60a5c660040e823b7/models/image_classification_mobilenet/LICENSE).
The photos and what is made of them stay on the computer.

**Shift+D** shows them together, the newest first; ⏎ opens one, ← → go
through them alone, Esc comes back. **X** (Not a screen) says the one the
keys are on is not. **Hide them from the grid** in the side panel, or on
that view, keeps them out of the grid, and so out of the Rainbow wall,
Mosaic, the Photo frame and On this day, which leave out what the grid
hides. On a photo shown full window, **Photo of a screen** (under More)
says it is or is not one. Every step can be undone with Ctrl Z.

What you say is kept with the library (`pinhole.synced`, `is<TAB><id>`:
true or false), so it is so on your other computers too (an iCloud
library's through iCloud Drive), and it teaches the finder there and here:
a photo alike one you said of goes its way, the more the more alike
(cosine likeness of the model's numbers from 0.55 up), so a scene shot
again, the same machine's display or the same page follows. It does not learn
games in general from a few: in a simulation on the author's library,
twenty photos said to be screens brought in only a few more, and twenty
said not to be took away a few like them and nothing else.

## How well it finds them

Measured on the author's iCloud library (46,350 photos with a thumbnail,
2026-11-25). A larger model (WeMM 4B, the photo-content search's, on the
GPU) was the teacher: its numbers of every photo, grouped, showed which
groups were photographed screens; a linear probe from those, looked at in
bands, put about 1,500–1,700 photographed screens in the library (3.5%).
The head was fitted to that teacher's likelihoods (`train/train-head`).

- **What it finds:** 1,789 photos (besides screenshots) in that library.
  82 of 96 picked at random were screens by eye (about 85%). Fitted on one
  half of the library and asked of the other, it found 2,000 of which
  about 77% were screens: what to expect of a library it has never seen.
- **What it mistakes for one:** printed tables and invoices, barcode
  labels, circuit boards, 3D printers and their prints, a lit sign.
- **What it misses:** about one in ten of the photos the teacher was sure
  of, nearly all games photographed so close that no edge of the screen
  or its glare shows (a whole frame of a game looks like a painting to
  it). Saying so on one brings in its near twins (the same scene shot
  again), not the game's other scenes.
