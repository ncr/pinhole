# People

People is off until turned on in the side panel. The shared `pinholed`
service then indexes still thumbnails in the current source, even with the
window closed. It uses Python 3 with OpenCV and NumPy, two CPU threads and
lower scheduling priority. On the first scan it downloads and verifies the
two public model files (39 MB). Photos, faces and their vectors stay on the
computer. **The names you give, and which faces you said are or are not
someone, are not local:** they go with the library to every computer it is
shown on, and for an iCloud library they are written to a `Pinhole` folder
in that account's iCloud Drive: whoever that folder is shared with can read
them. **Names only on this computer** (Settings → Plugins → People) keeps
them in People's state on this computer instead
(`~/.local/state/pinhole/plugins/pinhole.people/state.json`): turning it on
moves those of the library shown there and takes them off the other
computers; turning it off moves them back.

Choose **People** in the grid actions or press Shift+F. The people named
so far come first, each a round face with their photo count, then the
faces not named yet, the largest groups first; the list grows as you
scroll. Arrows move across both, ⏎ opens, Esc goes back.

A group opens to its photos with the names already given above them, the
likeliest first, each on a number key: 1 names the whole group, and the
next group to name opens. Type a new name in **New name…** (N). 0
(**Don't know**) skips a face you do not recognise: it waits under **Not
known yet** at the end of the page, to be named whenever you remember. X (or the
✕ on a photo under the pointer) takes a photo that is someone else out of
the group.

Once someone has a name, groups that look like them are marked
**Name?** at home, and their page offers **N more to check** (C): one group
at a time, 1 says yes, 0 says no and is not asked again, S skips. On a
person's page X takes a photo away from them, M (or …) gives it to
someone else, and **Merge with…** joins two names. Every step can be undone
with Ctrl Z. A photo opened from a group or a person steps ← → through
those photos alone.

On a photo shown full window, a button beside the heart says who is in it:
the names given (**Anna, Ola**), who a face most likely is (**Anna?**), the
other faces counted (**+1**), or **Who is this?**. W (or the button) rings
each face on the photo and opens a card beside it for the first face not
named yet: ⏎ says **Yes** to the person it looks like, a name typed (one
given before, or a new one) names it, and **Not Anna** takes a wrong name
back, so it is not guessed to be them again. Each named face moves on to the
next one not named; after the last the card closes. A ring or a face on the
card chooses another; Esc closes it. A face is named alone, its group still
waiting in People, where it now counts towards that person. It works the
same on a photo opened from People.

Settings → Plugins → People sets how readily similar faces share a group.

The pinned YuNet and SFace weights come from
[OpenCV Zoo](https://github.com/opencv/opencv_zoo/tree/48b10fa505aeb51fbd127fc60a5c660040e823b7/models).
The [YuNet weights are MIT](https://github.com/opencv/opencv_zoo/blob/48b10fa505aeb51fbd127fc60a5c660040e823b7/models/face_detection_yunet/LICENSE)
and the [SFace weights are Apache 2.0](https://github.com/opencv/opencv_zoo/blob/48b10fa505aeb51fbd127fc60a5c660040e823b7/models/face_recognition_sface/LICENSE).
The helper verifies their SHA-256 digests before loading them.

The first pass groups around exemplar faces at a cosine similarity of
0.60. The setting in the side panel sets how the second pass works.
**Careful** (the default) compares up to six faces per group and requires
two independent matches at 0.62. **Wider** compares up to 32 faces per
group, checks two independent matches at 0.58 and considers stronger
pairs first. **Apart** keeps the first-pass groups. A group is shown as
probably someone named when the median of its six best faces' closest
matches among 48 of theirs is at least 0.5.
Groups that share a photo or have saved edits stay separate during this
pass. Changing the setting regroups the existing index in the background.
Inspect groups before naming them.
Face vectors and suggestions are in the plugin's per-source cache.
Names, assignments and rejections are kept with the library
(`pinhole.synced`), keyed by the face and its thumbnail bytes (a face taken
out of a group by `reject<TAB><group><TAB><face>`, so two computers that each
take another face out of one group keep both), so they are
on every computer the library is shown on: for an iCloud library they go
through a Pinhole folder in that account's iCloud Drive within a minute or
so, and each computer finds the same faces under the same keys in the same
thumbnails. They survive cache rebuilds with the same thumbnails and
recipe. When a photo's thumbnail changes (turned or cropped in iCloud,
edited on a phone), each face found again in it, as alike as faces grouped
together, keeps what was said of it: the finder keeps its old key as an
alias, and the plugin moves its edits to the new key.
