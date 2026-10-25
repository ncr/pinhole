#pragma once

class Library;
class Viewer;
class Outgoing;
class QString;

// PINHOLE_SELFTEST=viewer: open a HEIC photo, a JPEG, a video and a Live
// Photo in the viewer, report each stage's arrival, and quit.
void runViewerSelfTest(const Library &library, Viewer &viewer);

// PINHOLE_SELFTEST=outgoing: prepare a HEIC photo, a JPEG and a video as
// originals and in the compatible form, copy one to the clipboard, report.
void runOutgoingSelfTest(const Library &library, Outgoing &outgoing, const QString &folder);

class PhotoGrid;
// PINHOLE_SELFTEST=reload: open a photo and select two, wait for the service
// to announce a change, and check that all three followed their photos.
void runReloadSelfTest(const Library &library, PhotoGrid *grid, Viewer &viewer);

class QQuickWindow;
// PINHOLE_SELFTEST=mouse: drive the window with a mouse and a touchpad as a
// person would (right-click, open, the sides, the wheel, a sideways swipe,
// the back button, double-click) and report whether each did its job.
void runMouseSelfTest(QQuickWindow *window, PhotoGrid *grid, Viewer &viewer, const Library &library);
