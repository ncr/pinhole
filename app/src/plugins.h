#pragma once

#include "adjustment.h"

#include <QFileSystemWatcher>
#include <QHash>
#include <QJSValue>
#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <QPointer>
#include <QQmlError>
#include <QQmlPropertyMap>
#include <QJsonArray>
#include <QTimer>
#include <QVariantList>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <vector>

#include "display.h"
#include "keepawake.h"
#include "pluginstore.h"

class Favourites;
class Keymap;
class History;
class QProcess;
class Library;
class PhotoGrid;
class QJSEngine;
class QQmlEngine;
class QQuickItem;
class ServiceClient;
class Sources;
class Translator;
class Viewer;
namespace PluginQuery { class Control; }

// What a plugin sees of the photo shown full window (pinhole.viewer): read,
// and closed, never changed otherwise (what it hides is the window's).
class PluginViewer : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool open READ open NOTIFY openChanged)
    Q_PROPERTY(int index READ index NOTIFY indexChanged)
    Q_PROPERTY(QVariantMap hdr READ hdr NOTIFY hdrChanged)
    // The photo shown, as library.item() gives it, {} with none (API 14);
    // its id, "" with none. What library.item(viewer.index) was written for.
    Q_PROPERTY(QVariantMap photo READ photo NOTIFY photoChanged)
    Q_PROPERTY(QString id READ id NOTIFY photoChanged)
public:
    explicit PluginViewer(Viewer *viewer, class PluginLibrary *library, QObject *parent = nullptr);
    bool open() const;
    int index() const;
    QVariantMap hdr() const;
    QVariantMap photo() const;
    QString id() const;
    // The photo closed, as Esc closes it.
    Q_INVOKABLE void close();
signals:
    void openChanged();
    void indexChanged();
    void hdrChanged();
    void photoChanged();

private:
    Viewer *m_viewer;
    class PluginLibrary *m_library;
};

// What a plugin sees of the library: every photo by its position, oldest
// first by capture date, independent of the grid's timeline order.
class PluginLibrary : public QObject {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY changed)
    // The folder of the source's library: library.sqlite, read only, and
    // the thumbnails (docs/plugins.md).
    Q_PROPERTY(QString dir READ dir NOTIFY changed)
    // Which source's library it is, as the library itself says
    // ("icloud:me@example.com", "folder:/home/me/Pictures"), wherever it
    // is kept. For what a plugin keeps apart for each source, as Series
    // does its edits.
    Q_PROPERTY(QString key READ key NOTIFY changed)
    // Goes up whenever what item() and field() say may have changed: the
    // library read again, a favourite set (setFavorite, here or by another
    // plugin). For bindings over them.
    Q_PROPERTY(int revision READ revision NOTIFY revisionChanged)
public:
    explicit PluginLibrary(const Library *library, QObject *parent = nullptr);
    int count() const;
    QString dir() const;
    QString key() const;
    int revision() const { return m_revision; }
    // Something item() says changed in memory (a favourite).
    void touched();
    // The key of every source added, as the settings say now: what a
    // plugin keeps for one no longer there can go.
    Q_INVOKABLE QStringList keys() const;
    // A photo by its position or its id: index, id, date (ms), filename,
    // thumbnail (a URL), width and height (of the thumbnail), video, live,
    // favorite. Empty if there is no such photo.
    Q_INVOKABLE QVariantMap item(const QJSValue &photo) const;
    // One of item's fields for every photo, in the library's order: all
    // the dates at once, say, without a map made for each photo; or "day",
    // the day each was taken, in local time, as yyyymmdd. Empty for a
    // name it has not.
    Q_INVOKABLE QVariantList field(const QString &name) const;
    // The names field() knows, as `plugin check` holds a plugin to.
    static const QStringList &fieldNames();
    // A photo's position from its id, -1 if the library has none.
    Q_INVOKABLE int indexOf(const QString &id) const;
    // A position, or an id, as a position; -1 if neither is one here.
    int positionOf(const QJSValue &photo) const;
    // The library was read again: positions moved.
    void reloaded();
    // Read again, and plugins not yet told (reloaded): what they hold of
    // it is of the read before.
    bool stale() const;

signals:
    void changed();
    void revisionChanged();

private:
    const Library *m_library;
    int m_revision = 0;
    // The positions by id, of the library's read m_positionsOf.
    mutable QHash<QByteArray, int> m_positions;
    mutable quint64 m_positionsOf = 0;
    // The read plugins were last told of.
    quint64 m_told = 0;
};

// A plugin's settings as properties (pinhole.settings): read them, bind to
// them, assign them, and each is kept at once in plugins.json.
class PluginSettings : public QQmlPropertyMap {
    Q_OBJECT
public:
    explicit PluginSettings(class PluginHost *host);
    // Those it has: one set to null is taken out (QQmlPropertyMap keeps
    // a key once it has had it, as undefined).
    Q_INVOKABLE QStringList keys() const;

protected:
    QVariant updateValue(const QString &key, const QVariant &input) override;

private:
    PluginHost *m_host;
};

// What a plugin started and can stop: the promise fetch, run and record
// give has its cancel().
class PluginJob : public QObject {
    Q_OBJECT
public:
    explicit PluginJob(std::function<void()> cancel) : m_cancel(std::move(cancel)) {}
    Q_INVOKABLE void cancel();

private:
    std::function<void()> m_cancel;
};

// A plugin's side of the sources (pinhole.sources): the sources added and
// the one shown, as the window has them (Sources), and what it may change,
// only of the kinds its manifest's "sources" names: any other is rejected
// with "not_yours", so no plugin signs in or adds for another.
// docs/plugins.md describes it for plugin authors.
class PluginSources : public QObject {
    Q_OBJECT
    // The kinds its manifest names ("icloud").
    Q_PROPERTY(QStringList kinds READ kinds NOTIFY kindsChanged)
    // Every source added whose kind is on, as Sources lists them, and the
    // id of the one shown ("" for none).
    Q_PROPERTY(QVariantList list READ list NOTIFY listChanged)
    Q_PROPERTY(QString current READ current NOTIFY listChanged)
    // How the source shown is doing: fine, needs_sign_in, offline, failing;
    // no_service when no service answers, incompatible for another version.
    Q_PROPERTY(QString health READ health NOTIFY healthChanged)
    // A library being built, and when the one shown was last and is next
    // looked at (Sources).
    Q_PROPERTY(QVariantMap progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(QVariantMap rounds READ rounds NOTIFY roundsChanged)
    // What could be added of its kinds, by kind, as the service last found
    // (discover): { "icloud": { "icloudpd": [...] } }.
    Q_PROPERTY(QVariantMap discovered READ discovered NOTIFY discoveredChanged)
public:
    explicit PluginSources(class PluginHost *host);

    QStringList kinds() const;
    QVariantList list() const;
    QString current() const;
    QString health() const;
    QVariantMap progress() const;
    QVariantMap rounds() const;
    QVariantMap discovered() const;

    // Look again for what could be added.
    Q_INVOKABLE void discover();
    // Show a source added, by its id.
    Q_INVOKABLE void show(const QString &id);
    // A source of `kind` added from `entry` (what its kind takes: a folder's
    // path, a provider's own fields) and shown: a promise of the source as
    // shown (id, kind, name…).
    Q_INVOKABLE QJSValue add(const QString &kind, const QJSValue &entry);
    // Signing in to a source of `kind`: a promise of { state: "signed_in" }
    // (added and shown) or { state: "needs_code" }, then submitCode with
    // the code; rejected with the service's or the provider's code.
    Q_INVOKABLE QJSValue signIn(const QString &kind, const QJSValue &params);
    Q_INVOKABLE QJSValue submitCode(const QString &kind, const QJSValue &params);
    // Signing out of a source of its kinds: its session goes, it stays with
    // its photos here. A promise of the sources left.
    Q_INVOKABLE QJSValue signOut(const QString &id);

signals:
    void kindsChanged();
    void listChanged();
    void healthChanged();
    void progressChanged();
    void roundsChanged();
    void discoveredChanged();

private:
    // What the window asked the service, answered to the plugin as a
    // promise of its result (`shape` takes the part to give), unless the
    // plugin has stopped since.
    QJSValue ask(const std::function<void(std::function<void(bool, const QJsonObject &, const QString &, const QString &)>)> &how,
                 const std::function<QVariant(const QJsonObject &)> &shape);
    QJSValue refused(const QString &code, const QString &message);
    Sources *sources() const;
    PluginHost *m_host;
};

// The `pinhole` a plugin is given: the library, the photo the grid or the
// viewer is on, the larger renditions, programs to run (its own helpers,
// ffmpeg), a picture made into a GIF or a video, the clipboard, its
// settings, and a word at the foot of the window. docs/plugins.md describes
// it for plugin authors.
//
// What takes a while (fetch, run, record) gives a promise, with cancel()
// to stop it; given a callback in its place, as before API 2, it calls
// that instead. All of it ends with the plugin: turned off or loaded
// again, nothing it started answers any more.
class PluginHost : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString id READ id CONSTANT)
    // Which version of this API Pinhole has (Plugins::api).
    Q_PROPERTY(int api READ api CONSTANT)
    // The plugin's own folder, for its helpers and files.
    Q_PROPERTY(QString dir READ dir CONSTANT)
    // A folder for what it keeps and can make again, one for each source's
    // library.
    Q_PROPERTY(QString cacheDir READ cacheDir NOTIFY cacheDirChanged)
    // Its workers (the manifest's), as pinholed runs them (API 12): each
    // { name, running, since (ms), lastExit: { code, signal, at } or null,
    // key (the source's library it works for) }. workersChanged as they
    // start and end.
    Q_PROPERTY(QVariantList workers READ workers NOTIFY workersChanged)
    Q_PROPERTY(QObject *library READ library CONSTANT)
    Q_PROPERTY(QObject *viewer READ viewer CONSTANT)
    // The display the window is on (display.h): hdr, headroom, wide.
    Q_PROPERTY(QObject *display READ display CONSTANT)
    // An sRGB ICC profile installed with Pinhole, for a program to make a
    // picture sRGB with (magick -profile).
    Q_PROPERTY(QString srgbProfile READ srgbProfile CONSTANT)
    Q_PROPERTY(QObject *settings READ settings CONSTANT)
    // What it keeps that is not a setting (pluginstore.h): what the user
    // made, in ~/.local/state/pinhole/plugins/<id>/state.json, and what it
    // can work out again, in ~/.cache/pinhole/plugins/<id>/cache.json.
    // Each read the first time it is asked for.
    Q_PROPERTY(QObject *state READ state CONSTANT)
    Q_PROPERTY(QObject *cache READ cache CONSTANT)
    // What the user made of the library shown that goes with it to every
    // computer it is shown on (synced.h): People's names, Series' series
    // (API 14).
    Q_PROPERTY(QObject *synced READ synced CONSTANT)
    // The sources (PluginSources): those of its own kinds to add, sign in
    // to and out of.
    Q_PROPERTY(QObject *sources READ sources CONSTANT)
    // Photos ticked in the grid's select mode (a card's all of them).
    Q_PROPERTY(int selectionCount READ selectionCount NOTIFY selectionChanged)
    // How many photos the grid hides now, all plugins' `hidden` together.
    Q_PROPERTY(int hiddenCount READ hiddenCount NOTIFY hiddenChanged)
    // What last went wrong in it, for the side panel: its QML not loading,
    // an error thrown, a warning from one of its files. Empty when loaded
    // again.
    Q_PROPERTY(QString problem READ problem NOTIFY problemChanged)
    // The plugin over the photos now, one at a time (its id; "" for none,
    // this one's own when it is): one that would come back by itself
    // stays away while another's is.
    Q_PROPERTY(QString coveredBy READ coveredBy NOTIFY coveredByChanged)
    // A photo it opened to come back from (openPhotos' comeBack) is shown:
    // its view waits unseen, without the keys (API 11).
    Q_PROPERTY(bool away READ away NOTIFY awayChanged)
    // The keys are what was last used, not a pointer: show the ring on the
    // one they are on (API 11).
    Q_PROPERTY(bool keyboardInUse READ keyboardInUse NOTIFY keyboardInUseChanged)
    // What was used last: "keyboard", "mouse" or "touchpad" (API 12), as a
    // wheel's turn is told from a touchpad's swipe by it.
    Q_PROPERTY(QString input READ input NOTIFY keyboardInUseChanged)
    // Whether the keys are this plugin's now: it covers the photos, and no
    // panel of Pinhole's is over it (API 12).
    Q_PROPERTY(bool hasKeys READ hasKeys NOTIFY hasKeysChanged)
public:
    PluginHost(const QString &id, const QString &dir, class Plugins *plugins);
    ~PluginHost() override;

    QString id() const { return m_id; }
    int api() const;
    QString dir() const { return m_dir; }
    QString cacheDir() const;
    QVariantList workers() const;
    QObject *library() const;
    QObject *viewer() const;
    QObject *display() const;
    QString srgbProfile() const;
    QObject *settings() const { return m_settings.get(); }
    QObject *state() const;
    QObject *cache() const;
    QObject *synced() const;
    QObject *sources() const;
    QString problem() const { return m_problem; }
    QString coveredBy() const;

    // The photo under the pointer in the grid, else the one in the middle.
    Q_INVOKABLE int currentIndex() const;
    // The photos ticked, by position, oldest first.
    Q_INVOKABLE QList<int> selection() const;
    // The same by id, which a reload of the library does not move (API 10).
    Q_INVOKABLE QStringList selectionIds() const;
    int selectionCount() const;
    // Whether a photo (a position or an id) is hidden from the grid, by any
    // plugin's `hidden`: what a plugin counting or showing photos leaves
    // out, as the grid does.
    Q_INVOKABLE bool isHidden(const QJSValue &photo) const;
    int hiddenCount() const;
    // Scroll the grid to a photo (a position or an id), if it is not on
    // screen, and mark it there: the keyboard's cursor on it, and a ring
    // that fades. A hidden photo: the next one shown, where it would be.
    Q_INVOKABLE void reveal(const QJSValue &photo) const;
    // Open a photo (a position or an id) full window.
    Q_INVOKABLE void openPhoto(const QJSValue &photo) const;
    Q_INVOKABLE void openPhotoDirect(const QJSValue &photo) const;
    // As openPhotoDirect, with only `photos` (ids or positions) to step
    // through, in their order, until the viewer closes.
    // `options.comeBack`: its view waits away while the photo shows, and is
    // told `returned(id)` of the one last shown when it closes (API 11).
    Q_INVOKABLE void openPhotos(const QJSValue &photos, const QJSValue &photo, const QJSValue &options = {});
    bool away() const { return m_away; }
    bool keyboardInUse() const;
    QString input() const;
    bool hasKeys() const;
    // The viewer closed, or another plugin covered meanwhile.
    void cameBack(const QString &photo);
    void coveredMeanwhile() { if (m_away) m_awayInterrupted = true; }
    // A photo's file of `kind` (medium, full, original, video, live, and
    // a Live Photo's loop, bounce or long_exposure) from the service: a
    // promise of its path, rejected with an Error whose `code` is the
    // service's. With `done`, done(path) or done("", code). An object in
    // its place is `options`: `{ stream: true }` gives a URL for a
    // MediaPlayer instead of a path, the service's http:// one while it is
    // still downloading, else the file's.
    Q_INVOKABLE QJSValue fetch(const QJSValue &photo, const QString &kind, const QJSValue &done = {});
    // Photos (a list of positions or ids, or one) made favourites, or not
    // (favourites.h): at once in the library in memory (the grid's mark,
    // library.item().favorite), then in the source. A promise of
    // { changed }, the ids of those that were not so already, once the
    // service has set them; rejected with its code (unsupported,
    // other_source, needs_sign_in, unavailable, not_found, read_only,
    // no_service), the photos put back as they were; not_found too when
    // none of them is in the library. No undo step: the plugin's own
    // (did).
    Q_INVOKABLE QJSValue setFavorite(const QJSValue &photos, bool favorite);
    // Run a program, its own (in its folder) or any on PATH: a promise of
    // { stdout, stderr, code } once it exits 0, rejected with an Error
    // whose `code` is "exit" (and exitCode, stdout, stderr), "start",
    // "timeout" or "cancelled" (SIGTERM, then a kill a second later).
    // `options`: input (written to its stdin), timeout (ms), env (added to
    // Pinhole's), output(line) for each line it writes, as it comes (the
    // result's stdout is then what follows the last). With a function in place of
    // options: done(exitCode, stdout, stderr).
    Q_INVOKABLE QJSValue run(const QString &program, const QStringList &arguments, const QJSValue &options = {});
    // An item as it looks over a number of frames, made into a GIF or an
    // MP4 (ffmpeg): options are path, format ("gif" or "mp4"), width,
    // height, fps, frames, seek(frame), called before each frame is taken,
    // and progress(fraction) as it goes. A promise of the path. With
    // progress and done after the options: done(path, error) at the end.
    // The item must be in the window and showing while it records.
    Q_INVOKABLE QJSValue record(QQuickItem *item, const QJSValue &options, const QJSValue &progress = {},
                                const QJSValue &done = {});
    // A file on the clipboard, for pasting into a chat or a file manager:
    // an image as image data too, as Pinhole's own copy has it (Outgoing).
    // A promise of the path, once the clipboard has it (an image's PNG is
    // made first, away from the window's thread); rejected with "not_found"
    // if there is no such file.
    Q_INVOKABLE QJSValue copyFile(const QString &path);
    // Words on the clipboard, as text (paths, a link): a promise of them
    // once it has them (API 12). In a self-test the clipboard is the
    // window's own, as for copyFile.
    Q_INVOKABLE QJSValue copyText(const QString &text);
    // What the person did to a photo in Pinhole (API 13, adjustment.h):
    // { turn: quarters clockwise, crop: { x, y, width, height } as fractions
    // of the photo turned }. Never to its file: the grid, the viewer, an
    // export as JPEG and a copy show it so.
    Q_INVOKABLE QVariantMap adjustment(const QJSValue &photo) const;
    // Photos (positions or ids, or one) given this `turn` and/or `crop`;
    // { turn: 0, crop: null } takes it all back. False when there is no
    // library, or it could not be kept.
    // A promise of { photos: [{ id, turn, crop }], kept }: kept "source"
    // where the source keeps edits (iCloud: written there as its devices
    // write theirs, which they show and can revert), "here" where it keeps
    // none (a folder: Pinhole keeps it). Rejected "unavailable" when the
    // source could not take it.
    Q_INVOKABLE QJSValue adjust(const QJSValue &photos, const QVariantMap &adjustment);
    // Turned `quarters` more clockwise (-1: a quarter back), each from
    // where it is, its crop turning with it.
    Q_INVOKABLE QJSValue turn(const QJSValue &photos, int quarters);
    // A file in its own folder or its cache (a worker's), read whole away
    // from the window (API 13): a promise of its text, or of what its JSON
    // says; rejected "not_found", "not_yours" (another's file), "too_large"
    // (over 32 MB), "unreadable" or "json".
    Q_INVOKABLE QJSValue readText(const QString &path);
    Q_INVOKABLE QJSValue readJson(const QString &path);
    // The folder a file is in, in the file manager.
    Q_INVOKABLE void showFile(const QString &path) const;
    // ~/Pictures, or the desktop's own Pictures folder.
    Q_INVOKABLE QString picturesDir() const;
    // A file to make in Pictures/Pinhole, as Pinhole names what it makes:
    // `name.extension` there, or `name 2.extension`… if that is taken; the
    // folder made. In a self-test, the scratch Pictures (API 5).
    Q_INVOKABLE QString picturePath(const QString &name, const QString &extension) const;
    // The library's index (library.sqlite, docs/sources.md) asked, read
    // only, away from the window, with bounded time, rows and bytes:
    // a promise of its rows, each an object by column name; rejected
    // "sql", "no_library", "query_timeout", "query_limit", "query_busy"
    // or "cancelled". `values` bind its ?s
    // (API 5). Never sqlite3 on the file: that one opens it while the
    // service writes, and fails "database is locked".
    Q_INVOKABLE QJSValue query(const QString &sql, const QVariantList &values = {});
    // The ids of the photos the library says are screenshots (API 10).
    Q_INVOKABLE QJSValue screenshots();
    Q_INVOKABLE bool exists(const QString &path) const;
    // A file's size in bytes, and as people read it: "1.8 MB".
    Q_INVOKABLE double fileSize(const QString &path) const;
    // With `decimals` (API 3; 1 when left out).
    Q_INVOKABLE QString sizeText(double bytes, int decimals = 1) const;
    // A folder chosen in the desktop's own picker, titled `title`: a
    // promise of its path, rejected "cancelled" when none is.
    Q_INVOKABLE QJSValue chooseFolder(const QString &title);
    // The plugin's settings, kept in ~/.config/pinhole/plugins.json; the
    // same as pinhole.settings[key].
    Q_INVOKABLE QVariant setting(const QString &key, const QVariant &fallback = {}) const;
    // False if it could not be kept (the file could not be written).
    Q_INVOKABLE bool setSetting(const QString &key, const QVariant &value);
    // What pinhole.settings says of a setting never set (the manifest's
    // "settings", API 13): kept nowhere, so a later default reaches those
    // who never chose.
    void setSettingDefaults(const QVariantMap &defaults);
    // Done just now, to undo with Ctrl+Z: `name` in a word or two, and
    // functions that undo and redo it (history.h).
    // Its number, for dropStep (API 3).
    Q_INVOKABLE double did(const QString &name, const QJSValue &undo, const QJSValue &redo);
    // A step it did that did not happen after all (what it changed was
    // refused, and put back): out of the history, so Ctrl Z undoes what was
    // done before it, and no longer offered at the foot of the window.
    Q_INVOKABLE void dropStep(double step);
    // A few words at the foot of the window, for a while: "Saved to
    // Pictures".
    Q_INVOKABLE void notify(const QString &text);
    // The screen kept on, no screensaver and no lock, while `on`, as a
    // photo frame left running wants; false lets it go. Let go by itself
    // when the plugin is turned off or loaded again, and with Pinhole.
    // Whether the screen is kept on now: not where Pinhole cannot (not on
    // Wayland, the window hidden), which it then does once it can.
    Q_INVOKABLE bool keepAwake(bool on);
    // Something gone wrong, said under the plugin in the side panel and on
    // stderr: an Error (where it was thrown is said too) or a few words.
    // A promise's chain ends with .catch(pinhole.warn), as what a then()
    // throws is otherwise never said. An error coded `cancelled` or
    // `reloaded` is not said: neither is a fault.
    Q_INVOKABLE void warn(const QJSValue &error);

    // What went wrong, said in the side panel (and on stderr, unless the
    // engine said it there already).
    void report(const QString &problem, bool print = true);
    // Loaded again or turned off: what it started stops, nothing answers
    // its old self, and what went wrong before is forgotten.
    void stop();
    // Going with the window: stopped, and its programs still ending handed
    // over, to wait for with every other plugin's (endPrograms).
    std::vector<QProcess *> close();
    // Programs asked to end, given a second all told, then killed.
    static void endPrograms(const std::vector<QProcess *> &programs);
    // A program of a plugin's asked to end (SIGTERM), or killed, with all
    // it started: its process group (run() gives it one of its own).
    static void endProgram(QProcess *process);
    static void killProgram(QProcess *process);
    // A callback of the plugin's, unless it has since stopped; what it
    // throws is reported.
    void call(const QJSValue &function, const QJSValueList &args = {});
    // The settings file changed under it (setSetting, another write).
    void settingsChanged();

signals:
    void cacheDirChanged();
    void selectionChanged();
    // What the grid hides changed (or moved, the library read again).
    void hiddenChanged();
    void problemChanged();
    void notified(const QString &text);
    void coveredByChanged();
    // The plugin over this one handed a photo over (pinhole.openPhoto,
    // reveal): this one, covering beneath it, closes for good, not coming
    // back once the photo is closed.
    void dismissed();
    void awayChanged();
    void keyboardInUseChanged();
    void hasKeysChanged();
    void workersChanged();
    // What one of its workers said: a line of JSON with an "event" it wrote
    // on its stdout (API 12), its latest of each quarter second. What it
    // found, to read again from cacheDir, rather than ask on a timer.
    void workerSaid(const QString &name, const QVariantMap &said);
    // Back from the photos it opened (openPhotos' comeBack): `photo` is the
    // id of the one shown last.
    void returned(const QString &photo);

private:
    friend class PluginSettings;
    friend class PluginSources;
    QJSEngine *engine() const;
    // A promise to give the plugin: its resolve and reject, and `job`'s
    // cancel() as the promise's.
    struct Deferred { QJSValue promise, resolve, reject; };
    Deferred defer(const std::function<void()> &cancel);
    QJSValue error(const QString &code, const QString &message) const;
    // Settles a promise of the plugin's, unless it has stopped since
    // `generation`.
    void settle(int generation, const QJSValue &how, const QJSValue &value);
    QJSValue readFile(const QString &path, bool json);
    QList<QByteArray> idsOf(const QJSValue &photos) const;
    Adjustment currentAdjustment(const QByteArray &id) const;
    QJSValue applyAdjustments(const Adjustments &targets);

    QString m_id;
    QString m_dir;
    Plugins *m_plugins;
    std::unique_ptr<PluginSettings> m_settings;
    QVariantMap m_settingDefaults;
    mutable std::unique_ptr<PluginStoreMap> m_state, m_cache;
    mutable std::unique_ptr<class PluginSyncedMap> m_synced;
    mutable std::unique_ptr<PluginSources> m_sources;
    QString m_problem;
    bool m_away = false;
    bool m_awayInterrupted = false;
    // Bumped by stop(): what was started before it does not answer.
    int m_generation = 0;
    // Programs and recordings under way, each with how to stop it, stopped
    // with the plugin.
    std::vector<std::pair<QPointer<QObject>, std::function<void()>>> m_running;
    std::vector<std::weak_ptr<PluginQuery::Control>> m_queries;
    void track(QObject *job, std::function<void()> stop);
    // Going with the window: the programs asked to end are waited for here,
    // as no event loop is left to kill them a second later.
    bool m_closing = false;
    std::vector<QProcess *> m_ending;
};

// A mark a plugin puts in the corner of grid cells (PinholePlugin.marks),
// beside Pinhole's own, with a switch of its own in the side panel's "In
// the grid": drawn from a field of the library every photo has, or from a
// list of photos the plugin gives. docs/plugins.md describes it for plugin
// authors; Plugins gathers them into markKinds for the grid.
class PluginMark : public QObject {
    Q_OBJECT
    // Unique in the plugin: its id is "<plugin id>/<name>", as its switch
    // is kept (MarkChoices).
    Q_PROPERTY(QString name READ name WRITE setName NOTIFY changed)
    // Its switch's words in the side panel.
    Q_PROPERTY(QString text READ text WRITE setText NOTIFY changed)
    // A shape of Pinhole's own ("video", "live", "heart", "stack"), or one
    // character of the theme's font, a Nerd Font glyph (marks.h).
    Q_PROPERTY(QString icon READ icon WRITE setIcon NOTIFY changed)
    // A theme colour's name (red, green, yellow, blue, magenta, cyan,
    // accent), or "#rrggbb".
    Q_PROPERTY(QString colour READ colour WRITE setColour NOTIFY changed)
    // Left to right in the corner: a favourite's 10, Pinhole's video 20,
    // Live 30, Pinhole's stack 90.
    Q_PROPERTY(int order READ order WRITE setOrder NOTIFY changed)
    // The photos whose library field is true ("live", "favorite",
    // "video"); or "" for `photos`.
    Q_PROPERTY(QString field READ field WRITE setField NOTIFY changed)
    // The photos it marks, by position or id (field ""): followed through
    // the library read again until it gives them again, as `hidden` is.
    Q_PROPERTY(QVariantList photos READ photos WRITE setPhotos NOTIFY photosChanged)
    // Its switch until it is first turned.
    Q_PROPERTY(bool defaultShown READ defaultShown WRITE setDefaultShown NOTIFY changed)
public:
    using QObject::QObject;
    QString name() const { return m_name; }
    void setName(const QString &name);
    QString text() const { return m_text; }
    void setText(const QString &text);
    QString icon() const { return m_icon; }
    void setIcon(const QString &icon);
    QString colour() const { return m_colour; }
    void setColour(const QString &colour);
    int order() const { return m_order; }
    void setOrder(int order);
    QString field() const { return m_field; }
    void setField(const QString &field);
    QVariantList photos() const { return m_photos; }
    void setPhotos(const QVariantList &photos);
    bool defaultShown() const { return m_defaultShown; }
    void setDefaultShown(bool shown);

signals:
    void changed();
    void photosChanged();

private:
    QString m_name, m_text, m_icon, m_colour = QStringLiteral("accent"), m_field;
    int m_order = 50;
    QVariantList m_photos;
    bool m_defaultShown = true;
};

// Plugins: what is added to Pinhole without changing it. Each is a folder
// with a manifest.json and a QML entry point, as Omarchy's are: Pinhole's
// own under its plugins folder, anyone's own in ~/.config/pinhole/plugins
// (one there with a built-in's id takes its place). Pinhole's own are on
// unless turned off; one's own start off, to be turned on in the side
// panel. A plugin's files changing on disk loads it again. docs/plugins.md
// has the rest.
class Plugins : public QObject {
    Q_OBJECT
    // Every plugin found, by id, in the order shown: Pinhole's own, then
    // one's own, each by name.
    Q_PROPERTY(QStringList ids READ ids NOTIFY idsChanged)
    // Bumped whenever anything about any plugin changes (at() then says).
    Q_PROPERTY(int revision READ revision NOTIFY changed)
    // Where one's own plugins go.
    Q_PROPERTY(QString personalDir READ personalDir CONSTANT)
    // The plugins covering the photos, in the order they came (Main.qml
    // says). The last is over the photos, coveredBy: its keys are the
    // "plugin" layer's, the others wait beneath, unseen. Each plugin has it
    // as pinhole.coveredBy.
    Q_PROPERTY(QStringList covering READ covering WRITE setCovering NOTIFY coveredByChanged)
    Q_PROPERTY(QString coveredBy READ coveredBy NOTIFY coveredByChanged)
    // The library has been read (or found missing): plugins load from then
    // on (Main.qml), so they find it whole as they start, nothing of it
    // still being read on another thread, and what they keep of it shows
    // in the first frame already.
    Q_PROPERTY(bool ready READ ready NOTIFY readyChanged)
    // As they do, all those on at once: the window gathers what they add
    // once they all have (allLoaded), not as each does.
    Q_PROPERTY(bool loadingAll READ loadingAll NOTIFY allLoaded)
    // Every kind of mark the grid can draw, Pinhole's own and the loaded
    // plugins' (PluginMark), in their order left to right: each { id,
    // plugin ("" for Pinhole's own), name, text, icon, colour (as given: a
    // theme colour's name or "#rrggbb"), order, field ("video", "live",
    // "favorite", "stack", or "" for a plugin's list of photos),
    // defaultShown }. At most 32, and 4 of any plugin.
    Q_PROPERTY(QVariantList markKinds READ markKinds NOTIFY markKindsChanged)
public:
    // The version of what a plugin is given. A manifest's "api" asks for
    // at least this; a plugin asking for more says it needs a newer
    // Pinhole. 2: promises, pinhole.settings, notify, photos by id. 3:
    // marks, pinhole.state and pinhole.cache. 4: photos in sRGB
    // (image://file, a photo's thumbPath; docs/hdr.md). 5: the kit
    // (ScrollList, ScrollGrid, ScrollArea, PinchZoom, Panel, Card,
    // Choices), pinhole.query and pinhole.picturePath. 6: holdSdr.
    // 7: ShellButton.checkable for accessible selected buttons.
    // 8: pinhole.openPhotoDirect. 9: pinhole.openPhotos. 10: ids in hidden
    // and stacks, pinhole.selectionIds, pinhole.screenshots. 11: coming back
    // from the photos (openPhotos' comeBack, away, returned), PhotoTiles,
    // PanelHeader, SettingNote and SettingSwitch, Choices.numbered and
    // keyboardInUse. 12: pinhole.copyText, input and hasKeys. 13:
    // pinhole.readText and readJson, a worker's `said` in pinhole.workers.
    // 14: pinhole.synced.
    static constexpr int api = 15;
    static constexpr int kMaxMarks = 32;
    static constexpr int kMaxMarksEach = 4;

    Plugins(const Library *library, ServiceClient *service, Viewer *viewer, Translator *translator, History *history,
            QObject *parent = nullptr);
    ~Plugins() override;

    // What a plugin's QML imports as Pinhole.Plugin: its root and its
    // buttons, and Pinhole's own pieces to draw as Pinhole does.
    static void registerTypes();
    // What is wrong with the plugin in `dir`, if anything; its manifest.
    static QString manifestProblem(const QString &dir, QJsonObject *manifest, QString *main);
    // What `plugin check` says of a manifest Pinhole loads all the same:
    // keys it never reads, numbers that are not whole, a pinhole. id.
    static QStringList manifestWarnings(const QString &dir, const QJsonObject &manifest);
    // An id its folders and files can be named after (core's usable_id).
    static bool usableId(const QString &id);
    // The source kinds a manifest names ("sources"), and what is wrong with
    // them: a kind of pinholed's own (no "provider") only in Pinhole's own
    // plugin of it, or one's own copy of that plugin.
    static QStringList sourceKinds(const QJsonObject &manifest);
    static QString sourcesProblem(const QString &dir, const QString &id, const QJsonObject &manifest);
    // What is wrong with the workers a manifest declares ("workers"), as
    // pinholed would refuse them: a name, a program in the folder it can
    // run, its args, and API 10.
    static QString workersProblem(const QString &dir, const QJsonObject &manifest);
    // What is wrong with the renditions a manifest says it makes ("makes"),
    // as pinholed would refuse them.
    static QString makesProblem(const QString &dir, const QJsonObject &manifest);
    // What the plugin in `dir` uses that came with an API after the first,
    // by the API that brought it, by its files' code, strings and comments
    // aside: pinhole's ("pinhole.notify"), a photo's avgColor, or its
    // root's ("hidden"), for 2; "marks" or "pinhole.state" for 3.
    // `pinhole plugin check` warns of those of an API newer than its
    // manifest asks for.
    static QMap<int, QStringList> apiUses(const QString &dir);
    // The plugin in `dir`'s files named as one of Pinhole's own, or as one
    // of the `others`' (each its id and folder): a qsTr() in it may be
    // given that one's words (Translator). `pinhole plugin check` warns.
    // Pinhole's own keys a plugin's could clash with, read from the
    // window's QML: the key as Qt names it, "grid", "viewer" or "" (both),
    // and what it does.
    struct OwnKey { QString key, layer, what; };
    static QList<OwnKey> pinholesKeys();
    // What Pinhole's own `key` does in `layer` ("" for both), "" if none.
    static QString pinholesKey(const QString &key, const QString &layer);
    static QStringList nameClashes(const QString &dir, const QList<std::pair<QString, QString>> &others);
    // What `pinhole plugin check` refuses in the plugin in `dir` (each
    // "file:line: what, and what to use instead"), and what it warns of:
    // the footguns Pinhole's own plugins hit (docs/plugins.md, "What check
    // looks for").
    struct Findings {
        QStringList errors, warnings;
    };
    static Findings footguns(const QString &dir);
    // Why pinhole.run refuses to run `program` with `arguments`, else "":
    // sqlite3 (pinhole.query), a clipboard tool (pinhole.copyFile) or
    // xdg-user-dir (pinhole.picturePath), by name or path, after env and
    // the like, or in a shell's line (`sh -c "…"`): what check refuses in
    // the code, however the name was made.
    static QString refusedRun(const QString &program, const QStringList &arguments);

    // The grid, once the window is there.
    void setGrid(PhotoGrid *grid);
    void setKeymap(Keymap *keymap);
    Keymap *keymap() const { return m_keymap; }
    // The window's sources, which pinhole.sources mirrors and asks.
    void setSources(Sources *sources);
    Sources *sourcesObject() const { return m_sources; }
    // The source kinds plugin `id`'s manifest names, on or not.
    QStringList sourceKindsOf(const QString &id) const;
    // What setFavorite sets through (the window's one).
    void setFavourites(Favourites *favourites);
    Favourites *favourites() const { return m_favourites; }
    // The engine plugins load in: what it keeps of a plugin's files is
    // dropped when they change, and what it warns of in them is theirs.
    void setEngine(QQmlEngine *engine);
    // Pinhole's own plugins' native QML modules, each built into its
    // plugin's qml/ (Pinhole.Internal.Info is Info's): found there only, never
    // in a plugin of one's own (docs/plugins.md, Trust).
    static void addModules(QQmlEngine *engine);
    QJSEngine *engine() const;
    // The library has been read the first time (or is not there yet).
    void libraryOpened();
    bool ready() const { return m_ready; }
    bool loadingAll() const { return m_loadingAll; }
    // The library was read again, or moved: `moved` has each old
    // position's new one, -1 for a photo gone.
    void libraryReloaded(const std::vector<int> &moved);
    // The name of the source's library among plugins' cacheDirs:
    // icloud-<apple id>, folder-<path>.
    QString cacheName() const;
    // What plugin `id` hides, as Main.qml gathers it: `given`, its
    // `hidden`, unless it gave the same before the library was read again
    // and has not given it since (`fresh`, its hiddenChanged): then the
    // same photos where they are now, until it does.
    Q_INVOKABLE QVariantList hiddenOf(const QString &id, const QVariantList &given, bool fresh);
    // The same of its `stacks` (`fresh`, its stacksChanged): each stack's
    // photos where they are now until it gives its own again, never one
    // plugin's of the positions before a reload and another's of after.
    Q_INVOKABLE QVariantList stacksOf(const QString &id, const QVariantList &given, bool fresh);
    // Plugin `id`'s marks (PinholePlugin.marks, PluginMark each), as it
    // loads and when it gives others; [] as it goes. Followed from then on
    // by their own signals: no gathering in QML.
    Q_INVOKABLE void setMarks(const QString &id, const QList<QObject *> &marks);
    QVariantList markKinds() const { return m_markKinds; }
    // Each of plugin `id`'s marks drawn from its photos, by name: those
    // photos where they are now, followed through a reload until it gives
    // its own again.
    Q_INVOKABLE QVariantMap marksOf(const QString &id) const;

    QStringList ids() const { return m_ids; }
    QStringList covering() const { return m_covering; }
    void setCovering(const QStringList &ids);
    QString coveredBy() const { return m_covering.isEmpty() ? QString() : m_covering.last(); }
    // The plugin `id` showed a photo (openPhoto) or took the grid to one
    // (reveal). The one over the photos doing so closes those covering
    // beneath it for good: each is told (PluginHost::dismissed).
    void handedOver(const QString &id);
    int revision() const { return m_revision; }
    static QString personalDir();
    static QString builtInDir();

    // id, name, description, version, author, personal, enabled, error
    // (it cannot load: its manifest), problem (it went wrong running),
    // sources (the source kinds its manifest names), copy (one's own in
    // place of Pinhole's own of the same id),
    // toured (its tour seen through or closed), main (the entry point's URL,
    // changing when its files change, so it loads again), host (its
    // PluginHost).
    Q_INVOKABLE QVariantMap at(const QString &id) const;
    Q_INVOKABLE void setEnabled(const QString &id, bool enabled);
    // Its tour seen: it no longer shows by itself (Tour.qml).
    Q_INVOKABLE void setToured(const QString &id);
    // The notice of new plugins (Main.qml) put off for some days: it stays
    // away until then, unless one not among these is new.
    Q_INVOKABLE void putOffNew(const QStringList &ids, int days);
    Q_INVOKABLE bool newPutOff(const QStringList &ids) const;
    // The folder one's own plugins go in, in the file manager (made first).
    Q_INVOKABLE void showPersonalDir() const;
    // Look for plugins again.
    Q_INVOKABLE void rescan();
    // Opt-in diagnostics for Main.qml's synchronous loader, not plugin API.
    Q_INVOKABLE void startupLoaded(const QString &id, double ms) const;
    // Whose file a URL is: a plugin's id, "" for none (Pinhole's own). The
    // help lists a plugin's commands under its name by it.
    Q_INVOKABLE QString idOf(const QString &url) const;
    // Its QML did not load: what the engine says of it is its problem.
    Q_INVOKABLE void loadFailed(const QString &id);
    // A PluginSource of a kind its manifest does not name: left out, and
    // said under it.
    Q_INVOKABLE void sourceRefused(const QString &id, const QString &kind);
    // One of its actions' keys is another's too: neither works (two
    // shortcuts on one key are ambiguous), so it is said under it.
    Q_INVOKABLE void keyClashed(const QString &id, const QString &message);
    // Its viewerLayer could not be made: why, under it.
    Q_INVOKABLE void layerFailed(const QString &id, const QString &message);
    // A QML error as a plugin's author reads it: the file in its folder,
    // the line, what is wrong.
    static QString describe(const QQmlError &error, const QString &dir);

    const Library *libraryData() const { return m_library; }
    PluginLibrary *library() const { return m_libraryApi.get(); }
    PluginViewer *viewerApi() const { return m_viewerApi.get(); }
    // pinholed's workers, as its `workers` event last said: every plugin's.
    // Where what the person did to the photos is kept (adjustment.h).
    void setAdjustments(class AdjustmentStore *store) { m_adjustmentStore = store; }
    class AdjustmentStore *adjustments() const { return m_adjustmentStore; }
    const QJsonArray &serviceWorkers() const { return m_workers; }
    ServiceClient *service() const { return m_service; }
    Viewer *viewerObject() const { return m_viewer; }
    // What the display can show (pinhole.display).
    ColourDisplay *display() { return &m_display; }
    PhotoGrid *grid() const { return m_grid; }
    History *history() const { return m_history; }
    // The screen kept on for plugins that ask (PluginHost::keepAwake).
    KeepAwake *keepAwake() { return &m_keepAwake; }
    QJsonObject settingsOf(const QString &id) const;
    bool setSettingOf(const QString &id, const QString &key, const QJsonValue &value);
    // What plugins keep that is not a setting (PluginHost::state, cache).
    PluginStore *stateStore() { return &m_stateStore; }
    // What is synced of the library shown (synced.h): the plugins'.
    class SyncedStore *syncedStore() { return m_syncedStore.get(); }
    PluginStore *cacheStore() { return &m_cacheStore; }
    // Where: ~/.local/state/pinhole/plugins, ~/.cache/pinhole/plugins;
    // and the tours seen, ~/.local/state/pinhole/toured.json.
    static QString stateRoot();
    static QString cacheRoot();
    static QString touredPath();
    // ~/.local/state/pinhole/new-plugins.json: the notice put off, until when.
    static QString putOffPath();
    // Every file of theirs written that is still to be.
    void flush();
    // A plugin's problem changed: the side panel says, once things settle.
    void touched();

signals:
    void idsChanged();
    void changed();
    // The grid's selection, and the photos it hides.
    void selectionChanged();
    void hiddenChanged();
    // A plugin revealed a photo: the window marks where it is.
    void revealed(int index);
    // A plugin's few words for the foot of the window (notify).
    void notice(const QString &id, const QString &text);
    void coveredByChanged();
    void readyChanged();
    // Those on as the library was first read have all loaded.
    void allLoaded();
    // The library was read again: what plugins hide and stack (hiddenOf,
    // stacksOf) is where its photos are now, to be gathered again.
    void positionsMoved();
    void markKindsChanged();

private:
    struct Plugin {
        QString id, name, description, version, author, dir, main, error;
        // The source kinds its manifest names.
        QStringList sources;
        // Its manifest's "settings": each setting's default.
        QVariantMap settingDefaults;
        // One's own with the id of one of Pinhole's own, which it replaces.
        bool copy = false;
        // Its files' names, times and sizes: a change loads it again.
        QString stamp;
        bool personal = false;
        int loads = 0;
        std::unique_ptr<PluginHost> host;
    };
    void scan();
    // The key of a plugin's switch in plugins.json: its id, or copy:<id>
    // for one's own copy of one of Pinhole's (core's is_on, the same).
    QString switchKey(const QString &id) const;
    bool writeConfig(QJsonObject config);
    // plugins.json as it is now, another window's changes in it: what a
    // change starts from, so that writing it loses none of theirs.
    QJsonObject latestConfig();
    // Another window changed plugins.json: its switches and settings here.
    void configChangedElsewhere();
    // What plugins kept in their settings that is not one (a list found,
    // the user's edits, a panel left open), and the tours seen: moved out
    // of plugins.json to where it belongs, once, as Pinhole starts.
    void moveOutOfSettings();
    // That move's keys back in `config`, if writing them where they now
    // belong failed.
    QJsonObject unmoved(QJsonObject config) const;
    bool enabled(const Plugin &plugin) const;
    // Each plugin, on or off, may bring its own translations (i18n/).
    void updateCatalogs();
    // The engine's warnings: those from a plugin's files are its problems.
    void warned(const QList<QQmlError> &warnings);

    const Library *m_library;
    ServiceClient *m_service;
    Viewer *m_viewer;
    Keymap *m_keymap = nullptr;
    Translator *m_translator;
    History *m_history;
    PhotoGrid *m_grid = nullptr;
    Favourites *m_favourites = nullptr;
    Sources *m_sources = nullptr;
    QQmlEngine *m_engine = nullptr;
    std::unique_ptr<PluginLibrary> m_libraryApi;
    std::unique_ptr<PluginViewer> m_viewerApi;
    QJsonArray m_workers;
    class AdjustmentStore *m_adjustmentStore = nullptr;
    // Before the plugins, so it outlives their hosts, which let it go.
    KeepAwake m_keepAwake;
    ColourDisplay m_display;
    std::vector<std::unique_ptr<Plugin>> m_plugins;
    QStringList m_ids;
    int m_revision = 0;
    QStringList m_covering;
    // plugins.json, read once and kept as written.
    QJsonObject m_config;
    // Written away from the window's thread; before the stores, which
    // write through it.
    KeptFiles m_files;
    PluginStore m_stateStore, m_cacheStore;
    std::unique_ptr<class SyncedStore> m_syncedStore;
    // The plugins whose tours were seen (toured.json).
    QJsonObject m_toured;
    // The notice of new plugins put off (new-plugins.json): "until", in ms
    // since the epoch, and "ids".
    QJsonObject m_putOff;
    // What moveOutOfSettings took out of plugins.json, until it is written
    // where it now belongs: 0 while it is being, 1 written, 2 failed.
    QJsonObject m_moved;
    std::shared_ptr<std::atomic<int>> m_moving;
    bool m_touching = false;
    bool m_ready = false;
    bool m_loadingAll = false;
    // The cacheDirs' library as plugins were last told (cacheName).
    QString m_cacheName;
    // The grid's hiddenChanged while plugins were not yet told of a
    // reload: told after.
    bool m_hiddenPending = false;
    // Each plugin's hidden and stacks as last given, and those photos'
    // positions now.
    struct Followed {
        std::vector<int> hiddenGiven, hidden;
        std::vector<std::vector<int>> stacksGiven, stacks;
    };
    std::map<QString, Followed> m_followed;
    // Each plugin's marks as given, and for a mark of photos those photos'
    // positions now.
    struct GivenMark {
        QPointer<PluginMark> mark;
        std::vector<int> photos;
    };
    std::map<QString, std::vector<GivenMark>> m_marks;
    QVariantList m_markKinds;
    // The mark each kind is, by its place in m_markKinds: null for one of
    // Pinhole's own, or one drawn from a field.
    std::vector<PluginMark *> m_markOf;
    // What was last said of each plugin's marks (PluginHost::problem).
    std::map<QString, QString> m_markProblems;
    // The kinds again from the marks given, and the bits of those of
    // photos for the grid.
    void updateMarkKinds();
    void updateMarkBits();
    std::vector<int> markPhotos(const QVariantList &photos) const;
    QFileSystemWatcher m_watcher;
    QTimer m_settle;
    // plugins.json's folder (it is replaced, not written in place).
    QFileSystemWatcher m_configWatcher;
    QTimer m_configSettle;
};

// pinhole plugin new <id> | check [folder]: a plugin begun, or looked over
// as Pinhole would load it, from a terminal. -1 when the arguments are not
// a plugin command.
int runPluginCommand(int argc, char *argv[]);
