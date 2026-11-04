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

Select a group to see the photos and the particular face on each photo.
Enter a name and choose **Save name**. Select a match and choose **Different
person** to remove it from the suggestion. Name changes and rejections
support Pinhole's undo. Tab and Shift+Tab move through controls, arrow keys
move through the focused results, Enter opens the selection, and Escape
goes back one level. Click a group, then use **Open photo** on the selected
match. Scroll the results with a wheel or two fingers.

The pinned YuNet and SFace weights come from
[OpenCV Zoo](https://github.com/opencv/opencv_zoo/tree/48b10fa505aeb51fbd127fc60a5c660040e823b7/models).
The [YuNet weights are MIT](https://github.com/opencv/opencv_zoo/blob/48b10fa505aeb51fbd127fc60a5c660040e823b7/models/face_detection_yunet/LICENSE)
and the [SFace weights are Apache 2.0](https://github.com/opencv/opencv_zoo/blob/48b10fa505aeb51fbd127fc60a5c660040e823b7/models/face_recognition_sface/LICENSE).
The helper verifies their SHA-256 digests before loading them.

The first version groups around exemplar faces using a cosine similarity
cutoff of 0.60 and at most one face per photo in a group. The cutoff is
provisional. Small faces, profiles and large changes in appearance can be
missed or split across suggestions. Inspect groups before naming them.
Face vectors and suggestions are in the plugin's per-source cache.
Names and rejected matches are in plugin state, keyed by the exemplar
photo and its thumbnail bytes. Names survive cache rebuilds with the same
thumbnail and recipe. Replacing the exemplar thumbnail can detach a name
from its suggestion.
