#include "recent.h"

#include "serviceclient.h"
#include "storage.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileSystemWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QTimer>

#include <cstdio>
#include <functional>
#include <memory>
#include <csignal>
#include <poll.h>
#include <sys/prctl.h>
#include <unistd.h>

namespace {

const char *kRecentUsage = R"(Usage: pinhole recent [--count <n>] [--follow]

The photos and videos added to the library shown most lately, newest first,
as one line of JSON. --follow says it again each time that changes.
)";

// The newest `count` of the library in `dir`, as recent.h says.
QJsonObject newest(const QString &dir, int count) {
    QJsonObject out{{"library", dir}, {"source", QString()}, {"items", QJsonArray()}};
    const QString path = dir + "/library.sqlite";
    if (!QFile::exists(path))
        return out;
    QJsonArray items;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", "recent");
        db.setDatabaseName(path);
        // Only the service writes a library.
        db.setConnectOptions("QSQLITE_OPEN_READONLY");
        if (db.open()) {
            const QSqlRecord schema = db.record("assets");
            // A folder's photos may have no added date: when they were
            // taken stands in for it.
            const QString added = schema.indexOf("added_date") >= 0 ? "added_date" : "NULL";
            QSqlQuery q(db);
            q.setForwardOnly(true);
            q.prepare(QStringLiteral("SELECT id, filename, %1, asset_date, thumb_path, thumb_w, thumb_h, avg_color, "
                                     "is_video, duration, is_live, favorite FROM assets "
                                     "ORDER BY coalesce(%1, asset_date) DESC, asset_date DESC, id LIMIT ?")
                          .arg(added));
            q.addBindValue(count);
            if (q.exec()) {
                while (q.next()) {
                    const QString thumb = q.value(4).toString();
                    const quint32 colour = q.value(7).isNull() ? 0x303030 : q.value(7).toUInt();
                    items.append(QJsonObject{
                        {"id", q.value(0).toString()},
                        {"name", q.value(1).toString()},
                        {"added", q.value(2).isNull() ? q.value(3).toLongLong() : q.value(2).toLongLong()},
                        {"taken", q.value(3).toLongLong()},
                        {"thumb", thumb.isEmpty() ? QString() : dir + '/' + thumb},
                        {"width", q.value(5).toInt()},
                        {"height", q.value(6).toInt()},
                        {"color", QStringLiteral("#%1").arg(colour & 0xffffff, 6, 16, QLatin1Char('0'))},
                        {"video", q.value(8).toBool()},
                        {"duration", q.value(9).toDouble()},
                        {"live", q.value(10).toBool()},
                        {"favorite", q.value(11).toBool()},
                    });
                }
            } else {
                std::fprintf(stderr, "pinhole: %s\n", qPrintable(q.lastError().text()));
            }
            QSqlQuery meta(db);
            if (meta.exec(QStringLiteral("SELECT value FROM meta WHERE key = 'source'")) && meta.next())
                out.insert("source", meta.value(0).toString());
        }
    }
    QSqlDatabase::removeDatabase("recent");
    out.insert("items", items);
    return out;
}

// The library the shared service shows, as its status says: a source of
// any kind, a plugin's too, whose library's name only the service knows.
// Asked and let go at once, so this holds no service open. Not answering
// in a few seconds: the one the settings name (Storage::library).
void shownLibrary(QObject *context, std::function<void(const QString &)> then) {
    auto *client = new ServiceClient(QString(), context);
    auto settled = std::make_shared<bool>(false);
    auto finish = [client, then, settled](const QString &dir) {
        if (*settled)
            return;
        *settled = true;
        client->deleteLater();
        then(dir.isEmpty() ? Storage::library() : dir);
    };
    QTimer::singleShot(3000, client, [finish] { finish({}); });
    client->call(QStringLiteral("status"), {}, [finish](bool ok, const QJsonObject &result, const QString &) {
        finish(ok ? result.value("library").toString() : QString());
    });
}

// --follow: the library's files watched (the index's write-ahead log is
// written with every change), the library shown looked up again now and
// then (another source shown), and a line only when what it says changed.
class Follower : public QObject {
public:
    explicit Follower(int count) : m_count(count) {
        m_settle.setSingleShot(true);
        // A sync writes in bursts: said once they have settled.
        m_settle.setInterval(400);
        connect(&m_settle, &QTimer::timeout, this, [this] { say(); });
        connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { m_settle.start(); });
        connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_settle.start(); });
        // Watched files are lost when replaced or removed (the log, on the
        // last close), and the source shown can change: looked at again.
        m_again.setInterval(30000);
        connect(&m_again, &QTimer::timeout, this, [this] { look(); });
        m_again.start();
        look();
    }

private:
    // Which library is shown, asked again, then said.
    void look() {
        shownLibrary(this, [this](const QString &dir) {
            if (dir != m_dir || m_watcher.files().size() < 2) {
                m_dir = dir;
                watch(dir);
            }
            say();
        });
    }

    void watch(const QString &dir) {
        if (!m_watcher.files().isEmpty())
            m_watcher.removePaths(m_watcher.files());
        if (!m_watcher.directories().isEmpty())
            m_watcher.removePaths(m_watcher.directories());
        if (QDir(dir).exists())
            m_watcher.addPath(dir);
        for (const char *name : {"/library.sqlite", "/library.sqlite-wal"})
            if (QFile::exists(dir + name))
                m_watcher.addPath(dir + name);
    }

    void say() {
        // No one reads any more (the pipe's other end closed): done.
        pollfd out{STDOUT_FILENO, 0, 0};
        if (poll(&out, 1, 0) > 0 && (out.revents & (POLLERR | POLLHUP))) {
            QCoreApplication::exit(0);
            return;
        }
        if (m_dir.isEmpty())
            return;
        const QByteArray line = QJsonDocument(newest(m_dir, m_count)).toJson(QJsonDocument::Compact);
        if (line == m_said)
            return;
        m_said = line;
        std::fputs(line.constData(), stdout);
        std::fputc('\n', stdout);
        // Whoever read it is gone: so is this.
        if (std::fflush(stdout) != 0)
            QCoreApplication::exit(0);
    }

    const int m_count;
    QString m_dir;
    QByteArray m_said;
    QFileSystemWatcher m_watcher;
    QTimer m_settle, m_again;
};

} // namespace

int runRecent(int argc, char *argv[]) {
    int count = 24;
    bool follow = false;
    for (int i = 2; i < argc; ++i) {
        const QByteArray arg = argv[i];
        bool ok = false;
        if (arg == "--follow") {
            follow = true;
        } else if (arg == "--count" && i + 1 < argc && (count = QByteArray(argv[i + 1]).toInt(&ok), ok) && count > 0 &&
                   count <= 500) {
            ++i;
        } else {
            std::fputs(kRecentUsage, stderr);
            return 2;
        }
    }
    QCoreApplication app(argc, argv);
    app.setApplicationName("pinhole");
    if (!follow) {
        shownLibrary(&app, [count](const QString &dir) {
            const QByteArray line = QJsonDocument(newest(dir, count)).toJson(QJsonDocument::Compact);
            std::fputs(line.constData(), stdout);
            std::fputc('\n', stdout);
            QCoreApplication::exit(0);
        });
        return app.exec();
    }
    // Whoever follows (the bar's shell) can go without a word, reloaded or
    // restarted: this goes with it rather than follow for no one.
    const pid_t parent = getppid();
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    if (getppid() != parent)
        return 0;
    Follower follower(count);
    return app.exec();
}
