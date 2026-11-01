// PINHOLE_SELFTEST=stacks: the plugins' stacks through a reload that moves
// every photo (selftest.h).

#include "selftest.h"

#include "library.h"
#include "libraryreloader.h"
#include "photogrid.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QQuickWindow>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTimer>

#include <cstdio>
#include <functional>
#include <memory>

namespace {

// Each stack of two or more in the grid, as its photos' ids.
QSet<QString> groupsOf(const PhotoGrid *grid, const Library &library) {
    QSet<QString> groups;
    for (const QVariant &stack : grid->stacks()) {
        QStringList ids;
        for (const QVariant &index : stack.toList())
            if (const int i = index.toInt(); i >= 0 && i < library.count())
                ids.append(QString::fromUtf8(library.at(i).id));
        if (ids.size() < 2)
            continue;
        ids.sort();
        groups.insert(ids.join(' '));
    }
    return groups;
}

// The first `n` photos of the library not `gone` already, taken out of it
// on disk, and added to `gone`.
void removeFirst(const Library &library, int n, QSet<QString> &gone) {
    QStringList ids;
    for (int i = 0; ids.size() < n && i < library.count(); ++i)
        if (const QString id = QString::fromUtf8(library.at(i).id); !gone.contains(id))
            ids.append(id);
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", "selfteststacks");
        db.setDatabaseName(library.dir() + "/library.sqlite");
        if (db.open())
            for (const QString &id : std::as_const(ids)) {
                QSqlQuery q(db);
                q.prepare(QStringLiteral("DELETE FROM assets WHERE id = ?"));
                q.addBindValue(id);
                q.exec();
            }
    }
    QSqlDatabase::removeDatabase("selfteststacks");
    for (const QString &id : std::as_const(ids))
        gone.insert(id);
}

} // namespace

void runStacksSelfTest(QQuickWindow *window, PhotoGrid *grid, const Library &library, LibraryReloader &reloader) {
    auto say = [](const QString &line) {
        std::printf("selftest: %s\n", qPrintable(line));
        std::fflush(stdout);
    };
    // It takes photos out of the library on disk: never the one the
    // settings name, only a copy given with --library.
    if (!QFileInfo(library.dir()).absoluteFilePath().startsWith(QStringLiteral("/tmp/"))) {
        say(QStringLiteral("FAIL refusing: %1 is not a scratch library in /tmp").arg(library.dir()));
        QCoreApplication::exit(2);
        return;
    }
    auto failures = std::make_shared<int>(0);
    auto since = std::make_shared<QElapsedTimer>();
    since->start();
    // The groups before, as ids, the photos taken out since left out.
    auto before = std::make_shared<QSet<QString>>();
    auto gone = std::make_shared<QSet<QString>>();
    auto expected = [before, gone] {
        QSet<QString> groups;
        for (const QString &group : std::as_const(*before)) {
            QStringList ids = group.split(' ');
            ids.removeIf([&](const QString &id) { return gone->contains(id); });
            if (ids.size() >= 2)
                groups.insert(ids.join(' '));
        }
        return groups;
    };
    auto check = [=, &library](const QString &when) {
        const QSet<QString> now = groupsOf(grid, library), want = expected();
        int wrong = 0;
        for (const QString &group : now)
            wrong += want.contains(group) ? 0 : 1;
        const bool ok = wrong == 0 && now.size() > 0;
        say(QStringLiteral("%1 %2 (%3 ms): %4 stacks, %5 not among the groups before")
                .arg(ok ? QStringLiteral("ok  ") : QStringLiteral("FAIL"), when)
                .arg(since->elapsed()).arg(now.size()).arg(wrong));
        *failures += ok ? 0 : 1;
    };

    // Each step once the reload it waits for is in: right then (after
    // every other handler of it), the next turn, and later.
    // Once the library read has `count` photos.
    auto onReload = std::make_shared<std::function<void()>>();
    auto count = std::make_shared<int>(-1);
    QObject::connect(&reloader, &LibraryReloader::reloaded, window, [onReload, count, &library] {
        if (*onReload && library.count() == *count)
            std::exchange(*onReload, nullptr)();
    });
    auto finish = [=] {
        say(QStringLiteral("%1 failed").arg(*failures));
        QCoreApplication::exit(*failures ? 1 : 0);
    };
    auto rapid = [=, &library, &reloader] {
        say(QStringLiteral("rapid reloads, one photo out each"));
        before->clear();
        *before = groupsOf(grid, library);
        gone->clear();
        *count = library.count() - 6;
        auto left = std::make_shared<int>(6);
        auto next = std::make_shared<std::function<void()>>();
        *next = [=, &library, &reloader] {
            removeFirst(library, 1, *gone);
            if (--*left == 0)
                *onReload = [=] {
                    QTimer::singleShot(300, window, [=] { check(QStringLiteral("rapid: 300 ms after the last")); });
                    QTimer::singleShot(6000, window, [=] {
                        check(QStringLiteral("rapid: 6 s after the last"));
                        finish();
                    });
                };
            reloader.reload();
            if (*left > 0)
                QTimer::singleShot(150, window, [next] { (*next)(); });
        };
        (*next)();
    };
    auto first = [=, &library, &reloader] {
        *before = groupsOf(grid, library);
        say(QStringLiteral("settled at %1 ms: %2 stacks").arg(since->elapsed()).arg(before->size()));
        *count = library.count() - 5;
        removeFirst(library, 5, *gone);
        say(QStringLiteral("took out the first 5 photos; reloading"));
        *onReload = [=] {
            check(QStringLiteral("right after the reload"));
            QTimer::singleShot(0, window, [=] { check(QStringLiteral("the next turn")); });
            QTimer::singleShot(500, window, [=] { check(QStringLiteral("500 ms on")); });
            QTimer::singleShot(1700, window, [=] { check(QStringLiteral("1.7 s on")); });
            QTimer::singleShot(6000, window, [=] {
                check(QStringLiteral("6 s on"));
                rapid();
            });
        };
        reloader.reload();
    };
    // Settled: the plugins' stacks there and the same for two seconds.
    auto last = std::make_shared<QSet<QString>>();
    auto steady = std::make_shared<int>(0);
    auto waiting = std::make_shared<QTimer>();
    waiting->setInterval(250);
    QObject::connect(waiting.get(), &QTimer::timeout, window, [=, &library] {
        const QSet<QString> now = groupsOf(grid, library);
        *steady = !now.isEmpty() && now == *last ? *steady + 1 : 0;
        *last = now;
        if (*steady >= 8) {
            waiting->stop();
            first();
        } else if (since->elapsed() > 60000) {
            waiting->stop();
            say(QStringLiteral("FAIL no stacks settled within a minute"));
            QCoreApplication::exit(1);
        }
    });
    waiting->start();
}
