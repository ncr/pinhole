// PINHOLE_SELFTEST=hidden:<dir>: photos a plugin hides, as the window
// opens and in the viewer (selftest.h).

#include "selftest.h"

#include "library.h"
#include "photogrid.h"
#include "viewer.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QQuickWindow>
#include <QTimer>
#include <qpa/qwindowsysteminterface.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <vector>

void runHiddenSelfTest(QQuickWindow *window, PhotoGrid *grid, const Library &library, Viewer &viewer, const QString &dir) {
    auto say = [](const QString &line) {
        std::printf("selftest: %s\n", qPrintable(line));
        std::fflush(stdout);
    };
    auto failures = std::make_shared<int>(0);
    auto check = [say, failures](bool ok, const QString &what) {
        say((ok ? QStringLiteral("ok   ") : QStringLiteral("FAIL ")) + what);
        *failures += ok ? 0 : 1;
    };
    auto since = std::make_shared<QElapsedTimer>();
    since->start();
    auto state = [grid, since] {
        return QStringLiteral("%1 ms: %2 cells, %3 hidden").arg(since->elapsed()).arg(grid->count()).arg(grid->hiddenCount());
    };
    // Hidden from the first frame on: what was kept is there as the
    // plugins load, before the window draws. The first frame's as it was
    // synchronized (the window's thread waiting meanwhile), said once it
    // has been swapped.
    auto synced = std::make_shared<std::atomic<int>>(-1);
    auto syncing = std::make_shared<QMetaObject::Connection>();
    *syncing = QObject::connect(window, &QQuickWindow::beforeSynchronizing, window, [=] {
        int none = -1;
        synced->compare_exchange_strong(none, grid->hiddenCount());
    }, Qt::DirectConnection);
    auto first = std::make_shared<QMetaObject::Connection>();
    *first = QObject::connect(window, &QQuickWindow::frameSwapped, window, [=] {
        QObject::disconnect(*first);
        QObject::disconnect(*syncing);
        check(*synced > 0, QStringLiteral("hidden in the first frame (%1 hidden; swapped at %2)").arg(int(*synced)).arg(state()));
    });
    for (int at : {100, 150, 300})
        QTimer::singleShot(at, window, [=] {
            window->grabWindow().save(QStringLiteral("%1/at-%2.png").arg(dir).arg(at));
            check(grid->hiddenCount() > 0, QStringLiteral("hidden at %1").arg(state()));
        });

    // Then the viewer: a photo with hidden ones on both sides, and ← and →
    // over them, the neighbours sliding in. Once some are hidden (with
    // nothing kept yet, once found) and the plugins have settled.
    auto viewing = std::make_shared<std::function<void()>>();
    auto waiting = std::make_shared<QTimer>();
    waiting->setInterval(250);
    QObject::connect(waiting.get(), &QTimer::timeout, window, [=] {
        if (grid->hiddenCount() > 0 || since->elapsed() > 30000) {
            waiting->stop();
            QTimer::singleShot(1500, window, [viewing] { (*viewing)(); });
        }
    });
    QTimer::singleShot(1000, window, [waiting] { waiting->start(); });
    *viewing = [=, &library, &viewer] {
        say(QStringLiteral("viewing at %1").arg(state()));
        const int n = library.count();
        auto shownFrom = [grid, n](int i, int step) {
            while (i >= 0 && i < n && grid->isHidden(i))
                i += step;
            return i >= 0 && i < n ? i : -1;
        };
        int photo = -1;
        for (int i = n - 2; i > 0 && photo < 0; --i)
            if (!grid->isHidden(i) && grid->isHidden(i - 1) && grid->isHidden(i + 1) && shownFrom(i - 1, -1) >= 0
                && shownFrom(i + 1, 1) >= 0)
                photo = i;
        if (photo < 0) {
            check(false, grid->hiddenCount() > 0
                             ? QStringLiteral("a shown photo with hidden ones on both sides")
                             : QStringLiteral("a shown photo with hidden ones on both sides: none is hidden; give it a "
                                              "plugins.json with Screenshots hiding (\"pinhole.screenshots\": {\"hide\": true})"));
            QCoreApplication::exit(1);
            return;
        }
        const int next = shownFrom(photo + 1, 1), previous = shownFrom(photo - 1, -1);
        say(QStringLiteral("photo %1, hidden %2-%3 after it and %4-%5 before; next shown %6, previous %7")
                .arg(photo).arg(photo + 1).arg(next - 1).arg(previous + 1).arg(photo - 1).arg(next).arg(previous));
        viewer.openAt(photo, grid->rectOf(photo));
        auto press = [window](int code) {
            QWindowSystemInterface::handleKeyEvent<QWindowSystemInterface::SynchronousDelivery>(window, QEvent::KeyPress, code, Qt::NoModifier);
            QWindowSystemInterface::handleKeyEvent<QWindowSystemInterface::SynchronousDelivery>(window, QEvent::KeyRelease, code, Qt::NoModifier);
        };
        // Each: the photo beside on that side, the key, the slide caught
        // under way and the photo it came to.
        struct Step {
            int key, from, to;
            const char *name;
        };
        auto steps = std::make_shared<std::vector<Step>>(std::vector<Step>{
            {Qt::Key_Right, photo, next, "right"}, {Qt::Key_Left, next, photo, "left"},
            {Qt::Key_Left, photo, previous, "left-again"}, {Qt::Key_Right, previous, photo, "right-again"}});
        auto run = std::make_shared<std::function<void(size_t)>>();
        *run = [=, &viewer](size_t k) {
            if (k == steps->size()) {
                viewer.close();
                QTimer::singleShot(800, window, [=] { QCoreApplication::exit(*failures ? 1 : 0); });
                return;
            }
            const Step s = (*steps)[k];
            const int side = s.key == Qt::Key_Right ? 1 : -1;
            check(viewer.index() == s.from && viewer.beside(side) == s.to,
                  QStringLiteral("%1: at %2, beside it %3 (want %4)").arg(s.name).arg(viewer.index()).arg(viewer.beside(side)).arg(s.to));
            press(s.key);
            QTimer::singleShot(60, window, [=, &viewer] {
                window->grabWindow().save(QStringLiteral("%1/%2-sliding.png").arg(dir, s.name));
                check(viewer.index() == s.to && std::abs(viewer.stripOffset()) > 1,
                      QStringLiteral("%1: on %2 (want %3), sliding in, %4 px to go").arg(s.name).arg(viewer.index()).arg(s.to).arg(viewer.stripOffset(), 0, 'f', 0));
                QTimer::singleShot(900, window, [=, &viewer] {
                    window->grabWindow().save(QStringLiteral("%1/%2-done.png").arg(dir, s.name));
                    check(viewer.index() == s.to && std::abs(viewer.stripOffset()) < 1,
                          QStringLiteral("%1: still on %2 (%3 px off)").arg(s.name).arg(viewer.index()).arg(viewer.stripOffset(), 0, 'f', 1));
                    (*run)(k + 1);
                });
            });
        };
        QTimer::singleShot(1200, window, [run] { (*run)(0); });
    };
}
