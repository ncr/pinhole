# People

People is enabled by default. The shared `pinholed` service indexes
still thumbnails in the current source, even with the window closed.
It uses Python 3 with OpenCV and NumPy, two CPU threads and lower
scheduling priority. On the first scan it downloads and verifies the
two public model files (39 MB). Photo data stays on the computer.

Choose **People** in the grid actions or press Shift+F to see progress
and browse results. The worker resumes from the local cache and handles
additions, edits and removals. Settings → Plugins → People controls both
the worker and the view. A library explicitly opened with `--library`
can display existing suggestions in its cache.

Open a suggestion in **To review**. Choose **Assign group** or select a
photo and choose **Assign selected photo**. Pick a saved person from the
list or type a new name. **Named people** collects photos from every group
assigned to the same person. Open a named person to move a selected photo
or merge the person with another one. Names saved by older versions appear
in the same list.

Choose **Different person** to remove an incorrect face from a suggestion.
Assignments, merges and rejections support Pinhole's undo. Tab and Shift+Tab
move through controls, arrow keys move through the focused results, Enter
opens the selection, and Escape goes back one level. Scroll results with a
wheel or two fingers.

The pinned YuNet and SFace weights come from
[OpenCV Zoo](https://github.com/opencv/opencv_zoo/tree/48b10fa505aeb51fbd127fc60a5c660040e823b7/models).
The [YuNet weights are MIT](https://github.com/opencv/opencv_zoo/blob/48b10fa505aeb51fbd127fc60a5c660040e823b7/models/face_detection_yunet/LICENSE)
and the [SFace weights are Apache 2.0](https://github.com/opencv/opencv_zoo/blob/48b10fa505aeb51fbd127fc60a5c660040e823b7/models/face_recognition_sface/LICENSE).
The helper verifies their SHA-256 digests before loading them.

The first pass groups around exemplar faces at a cosine similarity of
0.60. **Join groups** in **To review** sets how the second pass works.
**Careful** (the default) compares up to six faces per group and requires
two independent matches at 0.62. **Wider** compares up to 32 faces per
group, checks two independent matches at 0.58 and considers stronger
pairs first. **Off** keeps the first-pass groups.
Groups that share a photo or have saved edits stay separate during this
pass. Changing the setting regroups the existing index in the background.
Inspect groups before naming them.
Face vectors and suggestions are in the plugin's per-source cache.
Assignments and rejections are in plugin state, keyed by the face and its
thumbnail bytes. They survive cache rebuilds with the same thumbnails and
recipe.
