#pragma once

#include <QObject>
#include <QStringList>

#include <functional>

class Incoming;
class Keymap;
class Library;
class LibraryReloader;
class PhotoGrid;
class QQuickWindow;
class Viewer;
class Outgoing;
class QString;

// The library position of the newest asset matching an SQL condition on
// the index's `assets a`; -1 if none.
int findIndex(const Library &library, const QString &where);

// A slot for a signal a self-test hears by name (a QML property's notify):
// calls `f` at once, while it lives.
class Called : public QObject {
    Q_OBJECT
public:
    Called(std::function<void()> f, QObject *parent) : QObject(parent), m_f(std::move(f)) {}
public slots:
    void call() { m_f(); }

private:
    std::function<void()> m_f;
};
// Calls `f` whenever `object`'s `property` changes, until `context` goes.
void onChange(QObject *object, const char *property, QObject *context, std::function<void()> f);

// PINHOLE_SELFTEST=viewer: open a HEIC photo, a JPEG, a video and a Live
// Photo in the viewer, report each stage's arrival, and quit. A Live
// Photo's motion and effects are the Live Photos plugin's: its layer's,
// as L and choosing an effect have them; with the plugin off, its still.
void runViewerSelfTest(QQuickWindow *window, const Library &library, Viewer &viewer);

// PINHOLE_SELFTEST=outgoing: prepare a HEIC photo, a JPEG and a video as
// originals and in the compatible form, copy one to the clipboard, report.
void runOutgoingSelfTest(const Library &library, Outgoing &outgoing, const QString &folder);

class PhotoGrid;
// PINHOLE_SELFTEST=reload: open a photo and select two, wait for the service
// to announce a change, and check that all three followed their photos, and
// so did the marks plugins put on photos (all but the stack mark).
void runReloadSelfTest(const Library &library, PhotoGrid *grid, Viewer &viewer);

class QQuickWindow;
// PINHOLE_SELFTEST=mouse: drive the window with a mouse and a touchpad as a
// person would (right-click, open, the sides, the wheel, a sideways swipe,
// the back button, double-click) and report whether each did its job.
void runMouseSelfTest(QQuickWindow *window, PhotoGrid *grid, Viewer &viewer, const Library &library);
// PINHOLE_SELFTEST=keys: the window through its states (the grid and its
// More menu, select mode and its menu, the viewer, its menu and its More
// menu, a More menu closed by Esc, the side panel, the help and sign-in over
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
// or does not; report the Live button's state at each grab. Then ⇧ L opens
// the four effects; Bounce chosen with the keys goes round and round, over
// the photo zoomed in too; Long exposure shows; Esc stops the motion as the
// photo shrinks; a loop plays as its photo grows from its cell. With the
// Live Photos plugin off: no Live button or mark, L does nothing, the
// photo shows as its still. Either way, the loop, bounce and motion other
// plugins fetch (Background, Frame, Send) come. Exits 1 if any fails.
void runLiveSelfTest(QQuickWindow *window, PhotoGrid *grid, const Library &library, Viewer &viewer, const QString &folder);

// PINHOLE_SELFTEST=layers:<folder>: with the test plugin app/tests/layers
// among the plugins (PINHOLE_BUILTIN_PLUGINS), and offscreen, so that its
// animations run: its layer made once for each photo shown, lying on the
// photo opened, zoomed and panned, told it is no longer shown on ← and on
// Esc, gone once the viewer is; none for photos passing quickly; its
// action's complaint under its button, gone four seconds later; and an
// action of order -5 before Copy in select mode's bar. Frames in <folder>.
void runLayersSelfTest(QQuickWindow *window, PhotoGrid *grid, Viewer &viewer, const Library &library, const QString &folder);

// PINHOLE_SELFTEST=sources:<folder>: the sources as they look, a frame of
// each in <folder>: the first run (PINHOLE_SELFTEST_PICK, a key pressed on
// it: "Return", "2"), the side panel's rows, O and Delete asked on the one
// shown, Add a source, and iCloud's sign-in: an Apple ID and the password
// "two-factor" typed, the code boxes, a wrong code, the code 123456, and
// the card saying the source shown needs signing in. Against a service
// with a sign-in provider of its own only (PINHOLE_AUTH), never Apple.
void runSourcesSelfTest(QQuickWindow *window, const QString &folder);

// PINHOLE_SELFTEST=actionbar:<folder>: the newest Live Photo in a series
// full window; grab its bar, then with each menu open on its key (and the
// arrow down in one), Esc between; then select mode's bar and its menus.
void runActionBarSelfTest(QQuickWindow *window, PhotoGrid *grid, Viewer &viewer, const Library &library, const QString &folder);

// PINHOLE_SELFTEST=favourite:<folder>: open the newest photo and press F
// (the Favourites plugin's heart), then F again; then in select mode tick
// them all and press F, and F again. Report whether each showed at once
// and after the service answered (the library read again), and grab the
// bar each time; with the plugin off, F does nothing. It really is set:
// only on a scratch source.
void runFavouriteSelfTest(QQuickWindow *window, PhotoGrid *grid, Viewer &viewer, const Library &library, const QString &folder);

class Incoming;
// PINHOLE_SELFTEST=drop:<dir>: drag the files in PINHOLE_SELFTEST_DROP
// (separated by ':') over the window and drop them, as a file manager
// would; report whether the window lit up, how adding went and when the
// new photos were in the grid, with the window as images in <dir> at each
// step. They really are added: only on a scratch source.
void runDropSelfTest(QQuickWindow *window, const Library &library, Incoming &incoming, const QString &dir,
                     const QStringList &files);

// PINHOLE_SELFTEST=tour:<dir>: on a profile that has not seen them (a
// fresh $XDG_STATE_HOME each run: the tours seen are its
// pinhole/toured.json, which a run writes), wait
// for the New plugins card (more than one tour not seen, each listed
// ready) or a tour to show by itself. Later (Esc) leaves them unseen and
// nothing follows; at the next launch (as if) the card lists them again.
// ⏎ on Series' (or the first ready); step through the tour with → and
// grab each step into <dir>; check no tour or card follows and that it
// alone is seen; ask for Series' again, as the side panel does, Esc, and
// check none follows. Then one of the card's turned off and on again
// shows its tour by itself; and at the next launch, Skip all marks the
// rest seen. Fails on any.
void runTourSelfTest(QQuickWindow *window, Viewer &viewer, const QString &folder);

// PINHOLE_SELFTEST=covering (selftestcovering.cpp): one plugin over the
// photos at a time. D (On this day), ⏎ on a photo, H (Colours' Same
// colour), ⏎, Esc: Colours alone comes back, and Esc leaves for the grid;
// then Colours over On this day: the last covers, the other unseen, and
// Esc goes back to it; Colours over it again and ⏎: On this day closes for
// good, Esc comes back to Colours alone; the Year calendar over it and ⏎:
// the grid. In each, whose the keys are and no key shared. Then On this
// day again, and over it the help, the side panel, the undo notice and a
// plugin's notice: each above it, by the items' stacking and by the
// window grabbed. Needs photos taken on this day in other years.
void runCoveringSelfTest(QQuickWindow *window, Viewer &viewer, Keymap &keymap);

// PINHOLE_SELFTEST=series:<dir>: once Series has found its series, open the
// longest on its screen to put it right, over the viewer, as G does; go
// through it with → and the wheel, up close and back, grabbing each into
// <dir>; check the viewer beneath took none of it.
void runSeriesSelfTest(QQuickWindow *window, Viewer &viewer, const QString &folder);

// PINHOLE_SELFTEST=sheet:<dir>: seven photos ticked in select mode
// (PINHOLE_SHEET_COUNT for more), K for the Contact sheet; a collage, the
// dates, a grid, white, wide gaps, each grabbed into <dir> with how long its
// key held the window; then Enter, which fetches them and makes the file,
// copied and kept in Pictures/Pinhole; and Esc.
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
// window watching that (PINHOLE_THEME_DIR); without them it refuses.
void runThemeSelfTest(QQuickWindow *window, const Library &library, Viewer &viewer, const QString &folder);

// PINHOLE_SELFTEST=background:<dir>: the first photo of
// PINHOLE_SELFTEST_PHOTOS full window, B to make it the Background plugin's
// desktop background, Ctrl+Z, Ctrl+⇧Z, Ctrl+Z; then, with
// PINHOLE_SELFTEST_VIDEO (an MP4), Series' studio's B on it as if just
// made, the theme switched, and Ctrl+Z, which finds the background before
// in its own theme's folder. Each checked on Omarchy's link, the still at
// most 3840 px, and grabbed into <dir>. Never the desktop's: Omarchy in a
// home of its own (PINHOLE_OMARCHY_HOME, with Pictures in it, as
// XDG_PICTURES_DIR), stand-ins for omarchy-theme-bg-set and omarchy-shell
// in PINHOLE_OMARCHY_STUBS; without them it refuses.
void runBackgroundSelfTest(QQuickWindow *window, const Library &library, Viewer &viewer, const QString &folder);

// PINHOLE_SELFTEST=otherseries:<dir>: Series' edits and another source's
// library of the same ids, shown from elsewhere (as another window or a
// terminal would): photos 0-2 grouped in the library shown; the service
// taken to say it shows the other, which leaves nothing to undo before its
// library is read; the library in <dir> swapped in, and Ctrl+Z, which
// changes nothing; the first one shown again, still grouped. Its config is
// plugins.json, so with XDG_CONFIG_HOME a folder of its own.
void runOtherSeriesSelfTest(QQuickWindow *window, const Library &library, LibraryReloader &reloader, const QString &dir);

// PINHOLE_SELFTEST=frame: the Photo frame started from the grid on all the
// photos: the screen kept on (an idle inhibitor on the window, on
// Wayland), a fade held still halfway with both photos, going on, then
// stopped: the screen let go at once, the photo fading out with the frame
// and only then let go; shown again after a wait held still and stopped,
// starting afresh; a photo with no medium rendition shown sharp, as its
// full size. Exits 1 if any step fails.
void runFrameSelfTest(QQuickWindow *window, const Library &library);

// PINHOLE_SELFTEST=hidden:<dir> (selftesthidden.cpp): with a plugin hiding
// photos (Screenshots, in hide mode, what it found kept: run it on a
// plugins.json with "pinhole.screenshots": {"hide": true}, after a run
// that found them, or the first frame has none hidden): some hidden
// before the first frame and at 100, 150 and 300 ms, grabbed into <dir>;
// then a photo with hidden ones on both sides full window, → and ← over
// them both ways, each neighbour beside it before the key and sliding in
// after, grabbed under way and done. Exits 1 if any step fails.
void runHiddenSelfTest(QQuickWindow *window, PhotoGrid *grid, const Library &library, Viewer &viewer,
                       const QString &dir);

// PINHOLE_SELFTEST=stacks (selfteststacks.cpp): once the plugins' stacks
// (Series', Screenshots') have settled, the first five photos taken out of
// the library on disk and it read again: every stack in the grid is still
// one of the groups before, by its photos' ids, right after the reload,
// the next turn, and 0.5, 1.7 and 6 s on; then six reloads in a row, one
// photo out each, checked 300 ms and 6 s after the last. It changes the
// library: only a scratch copy in /tmp, given with --library (and a
// PINHOLE_SOCKET of its own). Exits 1 if any check fails.
void runStacksSelfTest(QQuickWindow *window, PhotoGrid *grid, const Library &library, LibraryReloader &reloader);

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
