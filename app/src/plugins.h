#pragma once

#include <QFileSystemWatcher>
#include <QHash>
#include <QJSValue>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QVariantList>

#include <memory>
#include <vector>

class Library;
class PhotoGrid;
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
public:
    explicit PluginLibrary(const Library *library, QObject *parent = nullptr);
    int count() const;
    QString dir() const;
    // id, date (ms), filename, thumbnail (a URL), width and height (of the
    // thumbnail), video, live, favorite.
    Q_INVOKABLE QVariantMap item(int index) const;
    // A photo's position from its id, -1 if the library has none.
    Q_INVOKABLE int indexOf(const QString &id) const;
    // The library was read again: positions moved.
    void reloaded();

signals:
    void changed();

private:
    const Library *m_library;
    mutable QHash<QByteArray, int> m_positions;
};

// The `pinhole` a plugin is given: the library, the photo the grid or the
// viewer is on, the larger renditions, programs to run (its own helpers,
// ffmpeg), a picture made into a GIF or a video, the clipboard, and a few
// settings of its own. docs/plugins.md describes it for plugin authors.
class PluginHost : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString id READ id CONSTANT)
    // The plugin's own folder, for its helpers and files.
    Q_PROPERTY(QString dir READ dir CONSTANT)
    // A folder for what it keeps and can make again, one for each source's
    // library.
    Q_PROPERTY(QString cacheDir READ cacheDir NOTIFY cacheDirChanged)
    Q_PROPERTY(QObject *library READ library CONSTANT)
    Q_PROPERTY(QObject *viewer READ viewer CONSTANT)
public:
    PluginHost(const QString &id, const QString &dir, class Plugins *plugins);

    QString id() const { return m_id; }
    QString dir() const { return m_dir; }
    QString cacheDir() const;
    QObject *library() const;
    QObject *viewer() const;

    // The photo under the pointer in the grid, else the one in the middle.
    Q_INVOKABLE int currentIndex() const;
    // Scroll the grid to a photo, if it is not on screen.
    Q_INVOKABLE void reveal(int index) const;
    // Open a photo full window.
    Q_INVOKABLE void openPhoto(int index) const;
    // A photo's file of `kind` (medium, full, original, video, live) from
    // the service: done(path) once here, done("", error) if not.
    Q_INVOKABLE void fetch(int index, const QString &kind, const QJSValue &done) const;
    // Run a program; done(exitCode, stdout, stderr) when it ends. A program
    // without a slash is looked for in the plugin's folder, then on PATH.
    Q_INVOKABLE void run(const QString &program, const QStringList &arguments, const QJSValue &done);
    // An item as it looks over a number of frames, made into a GIF or an
    // MP4 (ffmpeg): options are path, format ("gif" or "mp4"), width,
    // height, fps, frames, and seek(frame), called before each frame is
    // taken. progress(fraction) as it goes, done(path, error) at the end.
    // The item must be in the window and showing while it records.
    Q_INVOKABLE void record(QQuickItem *item, const QJSValue &options, const QJSValue &progress, const QJSValue &done);
    // A file on the clipboard, for pasting into a chat or a file manager.
    Q_INVOKABLE void copyFile(const QString &path) const;
    // The folder a file is in, in the file manager.
    Q_INVOKABLE void showFile(const QString &path) const;
    // ~/Pictures, or the desktop's own Pictures folder.
    Q_INVOKABLE QString picturesDir() const;
    Q_INVOKABLE bool exists(const QString &path) const;
    // A file's size in bytes, and as people read it: "1.8 MB".
    Q_INVOKABLE double fileSize(const QString &path) const;
    Q_INVOKABLE QString sizeText(double bytes) const;
    // The plugin's settings, kept in ~/.config/pinhole/plugins.json.
    Q_INVOKABLE QVariant setting(const QString &key, const QVariant &fallback = {}) const;
    Q_INVOKABLE void setSetting(const QString &key, const QVariant &value);

signals:
    void cacheDirChanged();

private:
    QString m_id;
    QString m_dir;
    Plugins *m_plugins;
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
    Plugins(const Library *library, ServiceClient *service, Viewer *viewer, Translator *translator,
            QObject *parent = nullptr);
    ~Plugins() override;

    // The grid, once the window is there.
    void setGrid(PhotoGrid *grid) { m_grid = grid; }
    // The engine plugins load in: what it keeps of a plugin's files is
    // dropped when they change.
    void setEngine(QQmlEngine *engine) { m_engine = engine; }
    // The library was read again, or moved.
    void libraryReloaded();

    QStringList ids() const { return m_ids; }
    int revision() const { return m_revision; }
    static QString personalDir();
    static QString builtInDir();

    // id, name, description, version, author, personal, enabled, error,
    // main (the entry point's URL, changing when its files change, so it
    // loads again), host (its PluginHost).
    Q_INVOKABLE QVariantMap at(const QString &id) const;
    Q_INVOKABLE void setEnabled(const QString &id, bool enabled);
    // The folder one's own plugins go in, in the file manager (made first).
    Q_INVOKABLE void showPersonalDir() const;
    // Look for plugins again.
    Q_INVOKABLE void rescan();

    const Library *libraryData() const { return m_library; }
    PluginLibrary *library() const { return m_libraryApi.get(); }
    ServiceClient *service() const { return m_service; }
    Viewer *viewerObject() const { return m_viewer; }
    PhotoGrid *grid() const { return m_grid; }
    QJsonObject settingsOf(const QString &id) const;
    void setSettingOf(const QString &id, const QString &key, const QJsonValue &value);

signals:
    void idsChanged();
    void changed();

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
    QJsonObject readConfig() const;
    void writeConfig(const QJsonObject &config) const;
    bool enabled(const Plugin &plugin) const;
    void watch();
    // Each plugin turned on may bring its own translations (i18n/).
    void updateCatalogs();

    const Library *m_library;
    ServiceClient *m_service;
    Viewer *m_viewer;
    Translator *m_translator;
    PhotoGrid *m_grid = nullptr;
    QQmlEngine *m_engine = nullptr;
    std::unique_ptr<PluginLibrary> m_libraryApi;
    std::vector<std::unique_ptr<Plugin>> m_plugins;
    QStringList m_ids;
    int m_revision = 0;
    QFileSystemWatcher m_watcher;
    QTimer m_settle;
};
