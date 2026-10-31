#pragma once

#include <QStringList>

class Incoming;
class Keymap;
class Library;
class LibraryReloader;
class PhotoGrid;
class QQuickWindow;
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
// PINHOLE_SELFTEST=keys: the window through its states (the grid, select
// mode and its menu, the viewer, the side panel, the help and sign-in over
// it, adding a source), saying in each whose the keys are and any key two
// live commands share; then every command some input cannot do. Fails on
// either.
class Keymap;
void runKeysSelfTest(QQuickWindow *window, PhotoGrid *grid, Viewer &viewer, Keymap &keymap);
// PINHOLE_SELFTEST=inputs:<dir>: on screen, what the keys, the mouse and the
// touchpad each get now, as images in <dir>: the grid's cursor, a page
// down, select mode with Range, the undo card
// and the side panel's focus on a switch.
void runInputsSelfTest(QQuickWindow *window, PhotoGrid *grid, const QString &dir);

// PINHOLE_SELFTEST=copytwice:<folder>: open a photo, click Copy, grab the
// window; wait; click Copy again, grab again; report the button's phases.
void runCopyTwiceSelfTest(QQuickWindow *window, PhotoGrid *grid, Viewer &viewer, const QString &folder);

// PINHOLE_SELFTEST=live:<folder>: open the newest Live Photo, press L (twice,
// quickly), and grab the window as the motion is asked for, comes and plays
// or does not; report the Live button's state at each grab.
void runLiveSelfTest(QQuickWindow *window, const Library &library, Viewer &viewer, const QString &folder);

// PINHOLE_SELFTEST=actionbar:<folder>: the newest Live Photo in a series
// full window; grab its bar, then with each menu open on its key (and the
// arrow down in one), Esc between; then select mode's bar and its menus.
void runActionBarSelfTest(QQuickWindow *window, PhotoGrid *grid, Viewer &viewer, const Library &library, const QString &folder);

// PINHOLE_SELFTEST=favourite:<folder>: open the newest photo and press F,
// then F again; then in select mode tick them all and press F, and F
// again. Report whether each showed at once and after the service
// answered (the library read again), and grab the bar each time. It really
// is set: only on a scratch source.
void runFavouriteSelfTest(QQuickWindow *window, PhotoGrid *grid, Viewer &viewer, const Library &library, const QString &folder);

class Incoming;
// PINHOLE_SELFTEST=drop:<dir>: drag the files in PINHOLE_SELFTEST_DROP
// (separated by ':') over the window and drop them, as a file manager
// would; report whether the window lit up, how adding went and when the
// new photos were in the grid, with the window as images in <dir> at each
// step. They really are added: only on a scratch source.
void runDropSelfTest(QQuickWindow *window, const Library &library, Incoming &incoming, const QString &dir,
                     const QStringList &files);

// PINHOLE_SELFTEST=tour:<dir>: on a profile that has not seen it, wait for
// a plugin's tour to show by itself, step through it with → and grab each
// step into <dir>; check it ended, closed the viewer it opened and is
// marked seen; then ask for Series' again, as the side panel does, and Esc.
void runTourSelfTest(QQuickWindow *window, Viewer &viewer, const QString &folder);

// PINHOLE_SELFTEST=series:<dir>: once Series has found its series, open the
// longest on its screen to put it right, over the viewer, as G does; go
// through it with → and the wheel, up close and back, grabbing each into
// <dir>; check the viewer beneath took none of it.
void runSeriesSelfTest(QQuickWindow *window, Viewer &viewer, const QString &folder);

// PINHOLE_SELFTEST=sheet:<dir>: seven photos ticked in select mode, K for
// the Contact sheet; a collage, the dates, a grid, white with wide gaps,
// each grabbed into <dir>; then Enter, which fetches them and makes the
// file, copied and kept in Pictures/Pinhole; and Esc.
void runSheetSelfTest(QQuickWindow *window, PhotoGrid *grid, const QString &folder);

// PINHOLE_SELFTEST=undo:<dir>: the longest series on its screen to put it
// right; X takes a photo out, Ctrl+Z puts it back, Ctrl+⇧Z takes it out
// again, Ctrl+Z twice, the second with nothing left to undo. Check the
// series' length after each and grab the window, its notice, into <dir>.
void runUndoSelfTest(QQuickWindow *window, Viewer &viewer, const QString &folder);

// PINHOLE_SELFTEST=theme:<dir>: each photo of PINHOLE_SELFTEST_PHOTOS (ids,
// by commas) full window, T to make the Theme plugin's Omarchy theme from
// it, then Ctrl+Z; check Omarchy's theme was set and set back each time,
// and grab the window, retinted and not, into <dir>. Run it with Omarchy in
// a home of its own (PINHOLE_OMARCHY_HOME, OMARCHY_THEME_HEADLESS=1) and the
// window watching that (PINHOLE_THEME_DIR), or the desktop's theme changes.
void runThemeSelfTest(QQuickWindow *window, const Library &library, Viewer &viewer, const QString &folder);

// PINHOLE_SELFTEST=otherseries:<dir>: Series' edits and another source's
// library of the same ids, shown from elsewhere (as another window or a
// terminal would): photos 0-2 grouped in the library shown; the service
// taken to say it shows the other, which leaves nothing to undo before its
// library is read; the library in <dir> swapped in, and Ctrl+Z, which
// changes nothing; the first one shown again, still grouped. Its config is
// plugins.json, so with XDG_CONFIG_HOME a folder of its own.
void runOtherSeriesSelfTest(QQuickWindow *window, const Library &library, LibraryReloader &reloader, const QString &dir);

// What a self-test drives.
struct SelfTestContext {
    QQuickWindow *window;
    PhotoGrid *grid;
    Library &library;
    Viewer &viewer;
    Incoming &incoming;
    Outgoing &outgoing;
    Keymap &keymap;
    LibraryReloader &reloader;
};

// PINHOLE_SELFTEST: start the self-test it names, `name` or `name:argument`
// (selftestrun.cpp has those that are only pictures of the window). False,
// and a line saying which there are, for a name that is none.
bool runSelfTest(const QString &test, const SelfTestContext &context);
