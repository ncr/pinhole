#pragma once

#include <QFileSystemWatcher>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QQmlPropertyMap>
#include <QStringList>

#include <map>

#include "pluginstore.h"

// What is kept of a library that follows it to every computer it is shown
// on (core/src/synced.rs; docs/plugins.md, Synced): each plugin's keys, a
// file to each in
// ~/.local/state/pinhole/synced/<kind>-<name>/, with when and on which
// computer each key was last set. A key set here is there at once, and
// merged into its file soon after, under the folder's lock, as pinholed
// merges what other computers did. A file that changes (another window,
// another computer) is read again and merged in: the later of each key
// wins, so nothing set here is lost to what was there before it.
class SyncedStore : public QObject {
    Q_OBJECT
public:
    explicit SyncedStore(QObject *parent = nullptr);
    ~SyncedStore() override;

    // The library shown (its folder, as Library::dir()): every plugin's
    // keys become that library's. Empty: none, nothing kept.
    void setLibrary(const QString &libraryDir);
    QString folder() const { return m_dir; }
    // A library's folder of them, and a plugin's file in it.
    static QString folderFor(const QString &libraryDir);
    static QString fileIn(const QString &folder, const QString &plugin);
    // This computer's id, made the first time (~/.local/state/pinhole/device),
    // the one pinholed uses.
    static QString device();
    // A plugin's values of a library, read now from its file. Any thread.
    static QJsonObject load(const QString &libraryDir, const QString &plugin);

    // A plugin's values now, read the first time asked for.
    QJsonObject values(const QString &plugin);
    // Set here at once and kept soon; null or undefined takes it out.
    void set(const QString &plugin, const QString &key, const QJsonValue &value);
    void set(const QString &plugin, const QJsonObject &changes);
    // Taken in from what a plugin kept before it was synced: each key the
    // library has never had anywhere, stamped older than any set, so a
    // key set on any computer, now or once heard of, wins over it.
    void adopt(const QString &plugin, const QJsonObject &values);
    // Everything set so far written.
    void flush();

signals:
    // Some of a plugin's keys changed: set here, in another window or on
    // another computer, or another library shown (all of its keys). Told
    // once a turn of the event loop, however many changed in it.
    void changed(const QString &plugin, const QStringList &keys);
    // What was set could not be kept.
    void failed(const QString &plugin);

private:
    struct Stamp {
        qint64 at = 0;
        QString by;
        bool operator<(const Stamp &o) const { return at != o.at ? at < o.at : by < o.by; }
    };
    // How a file is: one written over by renaming another onto it (as
    // every writer of these does) is another inode, so a change is never
    // missed for coming in the same moment at the same size.
    struct Seen {
        quint64 inode = 0;
        qint64 nanos = -1;
        qint64 size = -1;
        bool operator==(const Seen &) const = default;
    };
    static Seen seen(const QString &path);
    struct Doc {
        QJsonObject values;
        QHash<QString, Stamp> clock;
        // Its file as last read or written here: read again when it is not.
        Seen file;
    };
    struct Change {
        QJsonValue value;
        Stamp stamp;
    };
    void keep(const QString &plugin, const QString &key, const Change &change);
    static Doc read(const QString &path, bool *ok = nullptr);
    // `from` merged into `into`: the keys whose values changed.
    static QStringList merge(Doc &into, const Doc &from);
    Doc &doc(const QString &plugin);
    void reread();
    void writeChanged();
    void tell(const QString &plugin, const QStringList &keys);

    KeptFiles m_files;
    QString m_dir;
    QString m_device;
    std::map<QString, Doc> m_docs;
    // Set here and not yet written, by plugin.
    QHash<QString, QHash<QString, Change>> m_pending;
    bool m_writing = false;
    QHash<QString, QStringList> m_told;
    bool m_telling = false;
    QFileSystemWatcher m_watcher;
};

// A plugin's synced keys as properties (pinhole.synced), as pinhole.state
// is, of the library shown: read one, bind to one, assign one and it is
// kept, and goes to the other computers that library is shown on.
class PluginSyncedMap : public QQmlPropertyMap {
    Q_OBJECT
public:
    PluginSyncedMap(SyncedStore *store, const QString &plugin, QObject *parent = nullptr);
    // Those it has.
    Q_INVOKABLE QStringList keys() const;
    // All of them at once, as an object: for a plugin's worker or a
    // question to a program.
    Q_INVOKABLE QVariantMap values() const;
    // What it kept before it synced them (its state), taken in: only keys
    // the library has never had, and any set on any computer wins over
    // them, now or once heard of (API 14).
    Q_INVOKABLE void adopt(const QVariantMap &values);

signals:
    // Some keys changed, here or elsewhere, or another library is shown.
    void changed(const QStringList &keys);

protected:
    QVariant updateValue(const QString &key, const QVariant &input) override;

private:
    SyncedStore *m_store;
    QString m_plugin;
};
