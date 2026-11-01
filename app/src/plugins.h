#pragma once

#include <QFileSystemWatcher>
#include <QHash>
#include <QJSValue>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QQmlError>
#include <QQmlPropertyMap>
#include <QTimer>
#include <QVariantList>

#include <functional>
#include <memory>
#include <vector>

class History;
class Library;
class PhotoGrid;
class QJSEngine;
class QQmlEngine;
class QQuickItem;
class ServiceClient;
class Translator;
class Viewer;

// What a plugin sees of the library: every photo by its position, oldest
// first, as the grid has them.
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
public:
    explicit PluginLibrary(const Library *library, QObject *parent = nullptr);
    int count() const;
    QString dir() const;
    QString key() const;
    // A photo by its position or its id: index, id, date (ms), filename,
    // thumbnail (a URL), width and height (of the thumbnail), video, live,
    // favorite. Empty if there is no such photo.
    Q_INVOKABLE QVariantMap item(const QJSValue &photo) const;
    // One of item's fields for every photo, in the library's order: all
    // the dates at once, say, without a map made for each photo; or "day",
    // the day each was taken, in local time, as yyyymmdd. Empty for a
    // name it has not.
    Q_INVOKABLE QVariantList field(const QString &name) const;
    // A photo's position from its id, -1 if the library has none.
    Q_INVOKABLE int indexOf(const QString &id) const;
    // A position, or an id, as a position; -1 if neither is one here.
    int positionOf(const QJSValue &photo) const;
    // The library was read again: positions moved.
    void reloaded();

signals:
    void changed();

private:
    const Library *m_library;
    mutable QHash<QByteArray, int> m_positions;
};

// A plugin's settings as properties (pinhole.settings): read them, bind to
// them, assign them, and each is kept at once in plugins.json.
class PluginSettings : public QQmlPropertyMap {
    Q_OBJECT
public:
    explicit PluginSettings(class PluginHost *host);

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
    Q_PROPERTY(QObject *library READ library CONSTANT)
    Q_PROPERTY(QObject *viewer READ viewer CONSTANT)
    Q_PROPERTY(QObject *settings READ settings CONSTANT)
    // Photos ticked in the grid's select mode (a card's all of them).
    Q_PROPERTY(int selectionCount READ selectionCount NOTIFY selectionChanged)
    // How many photos the grid hides now, all plugins' `hidden` together.
    Q_PROPERTY(int hiddenCount READ hiddenCount NOTIFY hiddenChanged)
    // What last went wrong in it, for the side panel: its QML not loading,
    // an error thrown, a warning from one of its files. Empty when loaded
    // again.
    Q_PROPERTY(QString problem READ problem NOTIFY problemChanged)
public:
    PluginHost(const QString &id, const QString &dir, class Plugins *plugins);
    ~PluginHost() override;

    QString id() const { return m_id; }
    int api() const;
    QString dir() const { return m_dir; }
    QString cacheDir() const;
    QObject *library() const;
    QObject *viewer() const;
    QObject *settings() const { return m_settings.get(); }
    QString problem() const { return m_problem; }

    // The photo under the pointer in the grid, else the one in the middle.
    Q_INVOKABLE int currentIndex() const;
    // The photos ticked, by position, oldest first.
    Q_INVOKABLE QList<int> selection() const;
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
    // A photo's file of `kind` (medium, full, original, video, live) from
    // the service: a promise of its path, rejected with an Error whose
    // `code` is the service's. With `done`, done(path) or done("", code).
    Q_INVOKABLE QJSValue fetch(const QJSValue &photo, const QString &kind, const QJSValue &done = {});
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
    Q_INVOKABLE void copyFile(const QString &path) const;
    // The folder a file is in, in the file manager.
    Q_INVOKABLE void showFile(const QString &path) const;
    // ~/Pictures, or the desktop's own Pictures folder.
    Q_INVOKABLE QString picturesDir() const;
    Q_INVOKABLE bool exists(const QString &path) const;
    // A file's size in bytes, and as people read it: "1.8 MB".
    Q_INVOKABLE double fileSize(const QString &path) const;
    Q_INVOKABLE QString sizeText(double bytes) const;
    // The plugin's settings, kept in ~/.config/pinhole/plugins.json; the
    // same as pinhole.settings[key].
    Q_INVOKABLE QVariant setting(const QString &key, const QVariant &fallback = {}) const;
    // False if it could not be kept (the file could not be written).
    Q_INVOKABLE bool setSetting(const QString &key, const QVariant &value);
    // Done just now, to undo with Ctrl+Z: `name` in a word or two, and
    // functions that undo and redo it (history.h).
    Q_INVOKABLE void did(const QString &name, const QJSValue &undo, const QJSValue &redo);
    // A few words at the foot of the window, for a while: "Saved to
    // Pictures".
    Q_INVOKABLE void notify(const QString &text);
    // Something gone wrong, said under the plugin in the side panel and on
    // stderr: an Error (where it was thrown is said too) or a few words.
    // A promise's chain ends with .catch(pinhole.warn), as what a then()
    // throws is otherwise never said.
    Q_INVOKABLE void warn(const QJSValue &error);

    // What went wrong, said in the side panel (and on stderr, unless the
    // engine said it there already).
    void report(const QString &problem, bool print = true);
    // Loaded again or turned off: what it started stops, nothing answers
    // its old self, and what went wrong before is forgotten.
    void stop();
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

private:
    friend class PluginSettings;
    QJSEngine *engine() const;
    // A promise to give the plugin: its resolve and reject, and `job`'s
    // cancel() as the promise's.
    struct Deferred { QJSValue promise, resolve, reject; };
    Deferred defer(const std::function<void()> &cancel);
    QJSValue error(const QString &code, const QString &message) const;
    // Settles a promise of the plugin's, unless it has stopped since
    // `generation`.
    void settle(int generation, const QJSValue &how, const QJSValue &value);

    QString m_id;
    QString m_dir;
    Plugins *m_plugins;
    std::unique_ptr<PluginSettings> m_settings;
    QString m_problem;
    // Bumped by stop(): what was started before it does not answer.
    int m_generation = 0;
    // Programs and recordings under way, each with how to stop it, stopped
    // with the plugin.
    std::vector<std::pair<QPointer<QObject>, std::function<void()>>> m_running;
    void track(QObject *job, std::function<void()> stop);
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
public:
    // The version of what a plugin is given. A manifest's "api" asks for
    // at least this; a plugin asking for more says it needs a newer
    // Pinhole. 2: promises, pinhole.settings, notify, photos by id.
    static constexpr int api = 2;

    Plugins(const Library *library, ServiceClient *service, Viewer *viewer, Translator *translator, History *history,
            QObject *parent = nullptr);
    ~Plugins() override;

    // What a plugin's QML imports as Pinhole.Plugin: its root and its
    // buttons, and Pinhole's own pieces to draw as Pinhole does.
    static void registerTypes();
    // What is wrong with the plugin in `dir`, if anything; its manifest.
    static QString manifestProblem(const QString &dir, QJsonObject *manifest, QString *main);

    // The grid, once the window is there.
    void setGrid(PhotoGrid *grid);
    // The engine plugins load in: what it keeps of a plugin's files is
    // dropped when they change, and what it warns of in them is theirs.
    void setEngine(QQmlEngine *engine);
    QJSEngine *engine() const;
    // The library was read again, or moved.
    void libraryReloaded();

    QStringList ids() const { return m_ids; }
    int revision() const { return m_revision; }
    static QString personalDir();
    static QString builtInDir();

    // id, name, description, version, author, personal, enabled, error
    // (it cannot load: its manifest), problem (it went wrong running),
    // toured (its tour seen through or closed), main (the entry point's URL,
    // changing when its files change, so it loads again), host (its
    // PluginHost).
    Q_INVOKABLE QVariantMap at(const QString &id) const;
    Q_INVOKABLE void setEnabled(const QString &id, bool enabled);
    // Its tour seen: it no longer shows by itself (Tour.qml).
    Q_INVOKABLE void setToured(const QString &id);
    // The folder one's own plugins go in, in the file manager (made first).
    Q_INVOKABLE void showPersonalDir() const;
    // Look for plugins again.
    Q_INVOKABLE void rescan();
    // Its QML did not load: what the engine says of it is its problem.
    Q_INVOKABLE void loadFailed(const QString &id);
    // A QML error as a plugin's author reads it: the file in its folder,
    // the line, what is wrong.
    static QString describe(const QQmlError &error, const QString &dir);

    const Library *libraryData() const { return m_library; }
    PluginLibrary *library() const { return m_libraryApi.get(); }
    ServiceClient *service() const { return m_service; }
    Viewer *viewerObject() const { return m_viewer; }
    PhotoGrid *grid() const { return m_grid; }
    History *history() const { return m_history; }
    QJsonObject settingsOf(const QString &id) const;
    bool setSettingOf(const QString &id, const QString &key, const QJsonValue &value);
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

private:
    struct Plugin {
        QString id, name, description, version, author, dir, main, error;
        // Its files' names, times and sizes: a change loads it again.
        QString stamp;
        bool personal = false;
        int loads = 0;
        std::unique_ptr<PluginHost> host;
    };
    void scan();
    bool writeConfig(const QJsonObject &config);
    bool enabled(const Plugin &plugin) const;
    void watch();
    // Each plugin turned on may bring its own translations (i18n/).
    void updateCatalogs();
    // The engine's warnings: those from a plugin's files are its problems.
    void warned(const QList<QQmlError> &warnings);

    const Library *m_library;
    ServiceClient *m_service;
    Viewer *m_viewer;
    Translator *m_translator;
    History *m_history;
    PhotoGrid *m_grid = nullptr;
    QQmlEngine *m_engine = nullptr;
    std::unique_ptr<PluginLibrary> m_libraryApi;
    std::vector<std::unique_ptr<Plugin>> m_plugins;
    QStringList m_ids;
    int m_revision = 0;
    // plugins.json, read once and kept as written.
    QJsonObject m_config;
    bool m_touching = false;
    QFileSystemWatcher m_watcher;
    QTimer m_settle;
};

// pinhole plugin new <id> | check [folder]: a plugin begun, or looked over
// as Pinhole would load it, from a terminal. -1 when the arguments are not
// a plugin command.
int runPluginCommand(int argc, char *argv[]);
