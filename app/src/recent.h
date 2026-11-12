#pragma once

// `pinhole recent`: the photos added to the library shown most lately, as
// JSON, read straight from its index like the window reads it (no service
// asked). With --follow, a line again each time that changes, the index
// followed through its files: made for Omarchy's bar (omarchy-bar/), which
// shows what has just come from the phone.
//
// One JSON object per line:
//   {"library": dir, "source": "icloud:…", "items": [{"id", "name",
//    "added", "taken" (ms since 1970), "thumb" (absolute path, or empty),
//    "width", "height" (the thumbnail's), "color" ("#rrggbb", the photo's
//    average), "video", "duration" (s), "live", "favorite"}]}
// newest first.
int runRecent(int argc, char *argv[]);
