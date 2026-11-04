# People

A personal Pinhole plugin for reviewing and naming suggested groups of
similar faces. It uses plugin API 8 and a local Python 3 installation with
OpenCV (tested with 4.13.0) and NumPy. The helper runs on the CPU with two
threads and lower scheduling priority.

From the Pinhole checkout, fetch and verify the two public models (39 MB)
and link the plugin:

```sh
examples/people/pinhole-faces setup
mkdir -p "${XDG_CONFIG_HOME:-$HOME/.config}/pinhole/plugins"
ln -s "$PWD/examples/people" "${XDG_CONFIG_HOME:-$HOME/.config}/pinhole/plugins/me.people"
pinhole plugin check "$PWD/examples/people"
```

Enable **People** in Settings → Plugins. Choose **People** in the grid
actions or press Shift+F. Choose **Scan thumbnails** to process still
thumbnails in the current source. Closing the panel or pausing the scan
stops the worker. Scanning again skips thumbnails already analyzed and
checks for additions, edits and removals. It works offline after setup and
uses the thumbnails already present on disk.

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
