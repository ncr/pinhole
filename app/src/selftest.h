#pragma once

#include <QObject>
#include <QStringList>

#include <functional>

class Incoming;
class Keymap;
class Library;
class QMimeData;

// Safe by construction (selftestguard.cpp). A self-test (PINHOLE_SELFTEST
// set) never touches the person's own Pictures or clipboard:
// - The clipboard is private to the window: what Pinhole copies goes to
//   setClipboard(), which in a self-test keeps it in the process, and a
//   self-test reads it back with clipboardNow(). If anything still sets
//   the session's clipboard, the run ends at once with exit 3.
// - Pictures comes only from picturesLocation() (pinhole.picturesDir() for
//   plugins). In a self-test it is XDG_PICTURES_DIR when that is given
//   (Qt itself reads only user-dirs.dirs), else Qt's; and if it is the
//   account's own Pictures (~/Pictures of the home in the password file, or
//   the folder its real ~/.config/user-dirs.dirs names, whatever HOME says),
//   the run ends with exit 3 before anything is written.
// - guardSelfTest(), in main() before anything loads: every self-test
//   (Contact sheet, Series' studio, Copy, Background and the rest alike)
//   refuses to start, exit 3 and a line saying why, unless Pictures is a
//   scratch folder. Run them with HOME and XDG_PICTURES_DIR pointed at
//   scratch folders. It says where Pictures is as the first line.
bool selfTesting();
bool isAccountPictures(const QString &path);
QString picturesLocation();
void setClipboard(QMimeData *data);
const QMimeData *clipboardNow();
void guardSelfTest();
class LibraryReloader;
class PhotoGrid;
class QQuickWindow;
class Viewer;
class Outgoing;
class QString;

// The library position of the newest asset matching an SQL condition on
// the index's `assets a`; -1 if none.
int findIndex(const Library &library, const QString &where);
// Those of the newest, at most `most`, whose rendition `kind` ("full",
// "video") is on this computer: all of a folder's, iCloud's in the cache.
// A test that opens or copies one needs no network, nor a session.
QList<int> findHere(const Library &library, const QString &kind, const QString &where, int most);

// How the self-tests are run (each its own window, on workspace 5 for one
// drawn on screen): HOME's Pictures or XDG_PICTURES_DIR and every
// XDG_*_HOME a scratch folder of its own, a fresh state folder (no Apple
// session in it, ever), a copy of a real config, its
// own PINHOLE_SOCKET, and a stand-in sign-in provider (PINHOLE_AUTH) that
// never talks to Apple. On a library, --library a reflink copy of one
// (with its cache, in the same XDG_DATA_HOME). What each needs besides is
// said with it.
//
// A copy of an iCloud library is fetched for only by a service whose source
// is that library's (its meta says whose): with no Apple session in the
// scratch state there is none, and every fetch is turned down as another
// source's (other_source), so nothing past the thumbnails ever comes. So
// the scratch config gets a settings.json naming the copy's source
// ({"sources": [{"kind": "icloud", "account": "<apple id>"}], "source":
// ...}), and the copy lies anywhere but that source's own library path in
// XDG_DATA_HOME: there, the window would take it for the shared service's
// and start one that syncs. Given elsewhere, its service is a --given one,
// which only reads it and serves what is in the cache.

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
// the back button, double-click) and report whether each did its job. In
// a real window that draws its frames (the viewer's animations run on
// them): on workspace 5, floating, as a tiled window on a workspace not
// shown draws none. On a library whose photos are here: a folder's, or a
// copy of iCloud's with its cache; the photos stepped through, once the
// plugins' stacks have come, ten in a row in no stack, the one copied with
// its full size there, and the video played with its video, so neither
// waits on the network. With no session the source needs signing in: its
// card in the middle is looked past. Exits 1 if any step fails.
void runMouseSelfTest(QQuickWindow *window, PhotoGrid *grid, Viewer &viewer, const Library &library);
// PINHOLE_SELFTEST=keys: the window through its states (the grid and its
// More menu, select mode and its menu, the viewer, its menu and its More
// menu, a More menu closed by Esc, the side panel, the help and sign-in over
// it, adding a source), saying in each whose the keys are and any key two
// live commands share; then every command some input cannot do. Fails on
// either. With a source or none (Remove, on a source's row, then has
// nothing to remove), on any library.
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

// PINHOLE_SELFTEST=sourcesoff:<folder>: with the plugin of the source
// shown turned off at the start, without --library: the middle says so on
// its own backdrop, the grid empty and its buttons gone (frame 0-off, the
// side panel at its switch 1-side); 1 turns it on and the photos come
// back, how soon (2-on, once its listing has settled); turned off again
// while the window is open they go (3-off-again), and on again by its
// button they come back at once (4-on-again), and the source is followed,
// not listed again: its library's listed_at as before, a round since.
// Run it on a source that can be listed without signing in (a folder of a
// few photos as the settings' source, Folders off in plugins.json): an
// iCloud source with no session never has a round, and that fails.
// Exits 1 if any step fails.
void runSourcesOffSelfTest(QQuickWindow *window, const Library &library, const QString &folder);

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

// PINHOLE_SELFTEST=physics (selftestphysics.cpp): the side panel, the help
// and a plugin's screens scroll and zoom as the grid does. The same wheel
// notches, a spin, touchpad swipes with their coast, a drag set down, a
// Mac's momentum and a flick on a flick, given on one clock frame by frame
// to the grid and to the side panel and the help (each given 4000 px of
// room before and after, to be compared away from its ends), the Year
// calendar, On this day and Colours' photos of a colour (each a ScrollList
// or ScrollGrid), from the middle of each: their curves match
// within a pixel and a half until one reaches an end. A pinch out, held
// and let go, on the grid and on Colours' wall (a PinchZoom): the same size
// under the fingers, and the same easing to where each lands. Offscreen
// or anywhere: no frame of the window's drives them. Needs photos taken on
// this day in other years. Then a hard flick into the top of the grid and
// of the side panel: the bounce past the end goes a quarter of the view at
// most; a frame of each at its furthest in <dir>, if given.
void runPhysicsSelfTest(QQuickWindow *window, PhotoGrid *grid, const QString &dir = {});

// PINHOLE_SELFTEST=examples[:<dir>] (selftestphysics.cpp): the examples in
// docs/plugins.md, each a plugin in app/tests/examples, among the plugins
// (PINHOLE_BUILTIN_PLUGINS a folder with Pinhole's own and them), run: F5
// lists the years in a ScrollList, F6's tiles pinched out land a size up,
// F7's card makes a copy of the photo in Pictures/Pinhole and puts it on
// the (private) clipboard, F8 asks the index with pinhole.query. Each over
// the photos, as frames in <dir>. Pictures a scratch folder.
class Library;
void runExamplesSelfTest(QQuickWindow *window, PhotoGrid *grid, const Library &library, const QString &dir);

// PINHOLE_SELFTEST=through[:<dir>] (selftestthrough.cpp): nothing over the photos
// lets the pointer through to them, at 850 and 1700 wide. Over the date at
// the top left, over every part of the side panel (at its top, at its end
// and between, scrolled on past either), over the photos dimmed beside it,
// over the help, iCloud's sign-in, the New plugins card and select mode's
// Copy menu and beside each, and over the viewer: the wheel, a touchpad's
// swipes, flicks and momentum, smooth pixels, Ctrl and the wheel, the
// sideways wheel, pinches, two-finger taps, middle clicks; and where no
// control is, clicks, double clicks and touch taps. The grid neither
// scrolls, zooms, selects, opens nor drags anything out. And the panel
// itself: a notch glides it, a swipe moves it and coasts, and spun or
// flicked into either end it stops there, as the grid does: pulled less
// than its height past it, a coast bouncing on past it; a click or a touch tap on
// a switch while it coasts stops it and turns nothing. A plugin's settings
// with a list and a text of their own, each with a Scroller over it (the
// test plugin app/tests/scrolls, among the plugins: PINHOLE_BUILTIN_PLUGINS
// a folder with it): the wheel and a swipe over each move it, not the
// panel, until its end, then the panel. Begins once the plugins' stacks and hidden photos have come; a
// probe the grid was laid out again during, with nothing reaching it, is
// tried once more. With <dir>, the panel at either end and pulled on past
// it, as images there. Exits 1 if any fails.
void runThroughSelfTest(QQuickWindow *window, PhotoGrid *grid, Viewer &viewer, const QString &dir);

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

// PINHOLE_SELFTEST=colour:<dir> (selftestcolour.cpp): each photo of
// PINHOLE_SELFTEST_PHOTOS (ids, by commas; else the newest) in the grid at
// 3 columns, then full window as it opens and once its sharpest has come;
// the first at 13 and 29 columns (mosaics); then Colours' finder (H) and
// its rainbow wall (W). Frames in <dir>, named by the photo's file name
// and what they show, to compare their colours with a reference.
void runColourSelfTest(QQuickWindow *window, PhotoGrid *grid, const Library &library, Viewer &viewer, const QString &dir);

// PINHOLE_SELFTEST=hdrvideo:<dir> (selftesthdr.cpp): each video of
// PINHOLE_SELFTEST_VIDEOS (paths, by ':') as Qt Multimedia shows it, a
// MediaPlayer and VideoOutput as the viewer has them, paused on its first
// frame over the window; the frame grabbed into <dir> as <name>-qt.png,
// and beside it ffmpeg's tone mappings of the same frame (zscale and
// tonemap: hable at SDR white 203 nits, as cover.rs was; hable, mobius and
// clip at 100; and the codes as they are), each compared by the means of
// 16x9 blocks: the mean and the largest difference, out of 255.
void runHdrVideoSelfTest(QQuickWindow *window, const QString &dir, const QStringList &videos);
// PINHOLE_SELFTEST=hdrplay (selftesthdr.cpp): each video of
// PINHOLE_SELFTEST_VIDEOS played as Qt alone shows it (a VideoOutput) and
// as Pinhole does (VideoView), in a window of 960x540; a frame drawn every
// 16 ms (grabWindow, as a window on a workspace not shown draws none of
// its own). Over four seconds: frames given and drawn a second, the CPU
// the process took, and how far the frame given lags the player's clock;
// then a seek to 1 s, and where it plays a second later. Fails when VideoView
// draws under 55 frames a second of a 60 fps video, lags by over 50 ms, or
// a seek does not play on.
void runHdrPlaySelfTest(QQuickWindow *window, const QStringList &videos);

// PINHOLE_SELFTEST=stage:<dir> (selfteststage.cpp): the stage (stage.h,
// docs/hdr.md H3), with PINHOLE_STAGE=force or on a display that has it,
// read back as Pinhole hands it to the compositor (grabWindow of the stage:
// RGBA16F on an scRGB swapchain), frames in <dir> (.pfm, linear). A photo
// opened (PINHOLE_SELFTEST_PHOTOS: a photo, a Live Photo and a video, by
// ids; else the newest whose medium is here): the main window's SDR photo
// grabbed as the stage has it decoded; at the hand-over, the stage's
// pixels against those (fade 0), the backdrop the theme's in linear light,
// the main window transparent where the stage shows and its chrome
// opaque; once faded in, the photo's gamut beyond sRGB; a Display P3
// green (PINHOLE_SELFTEST_P3, the profile) as P3's green in linear sRGB;
// zoomed and panned, both windows' photo in the same place every step; a
// plugin's holdSdr, and Live Photos' motion played, the stage at SDR
// meanwhile; the viewer's video in the stage; an HLG clip
// (PINHOLE_SELFTEST_VIDEOS) through VideoView in the stage, above SDR
// white at the display's headroom and the main window's at 1; closed, the
// main window drawing the photo before the stage goes. Exits 1 if any
// check fails. In a compositor that draws frames: run it in a nested
// Hyprland on a headless output (docs/hdr.md).
void runStageSelfTest(QQuickWindow *window, const Library &library, Viewer &viewer, const QString &dir);

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
