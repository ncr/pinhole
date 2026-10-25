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
