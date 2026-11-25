// PINHOLE_SELFTEST=colour:<dir>: photos as the grid, the viewer and Mosaic
// draw them, to compare their colours with a reference (selftest.h).

#include "selftest.h"

#include "library.h"
#include "photogrid.h"
#include "viewer.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QQuickWindow>
#include <QTimer>
#include <qpa/qwindowsysteminterface.h>

#include <cstdio>
#include <functional>
#include <memory>

namespace {

void press(QQuickWindow *window, int code, const QString &text) {
    QWindowSystemInterface::handleKeyEvent<QWindowSystemInterface::SynchronousDelivery>(window, QEvent::KeyPress, code, Qt::NoModifier, text);
    QWindowSystemInterface::handleKeyEvent<QWindowSystemInterface::SynchronousDelivery>(window, QEvent::KeyRelease, code, Qt::NoModifier, text);
}

} // namespace

void runColourSelfTest(QQuickWindow *window, PhotoGrid *grid, const Library &library, Viewer &viewer, const QString &dir) {
    auto say = [](const QString &line) {
        std::printf("selftest: %s\n", qPrintable(line));
        std::fflush(stdout);
    };
    auto grab = [window, dir, say](const QString &name) {
        window->grabWindow().save(QStringLiteral("%1/%2.png").arg(dir, name));
        say(QStringLiteral("grabbed %1").arg(name));
    };
    // The steps, one after another, each after its wait.
    struct Step {
        int wait;
        std::function<void()> run;
    };
    auto steps = std::make_shared<std::vector<Step>>();
    auto next = std::make_shared<std::function<void(size_t)>>();
    *next = [window, steps, next](size_t i) {
        if (i >= steps->size()) {
            QCoreApplication::quit();
            return;
        }
        QTimer::singleShot((*steps)[i].wait, window, [steps, next, i] {
            (*steps)[i].run();
            (*next)(i + 1);
        });
    };
    auto clock = std::make_shared<QElapsedTimer>();
    clock->start();
    auto poll = new QTimer(window);
    poll->setInterval(250);
    QObject::connect(poll, &QTimer::timeout, window, [=, &library, &viewer] {
        window->setProperty("troubleSeen", true);
        if (library.count() == 0 && clock->elapsed() < 60000)
            return;
        poll->deleteLater();
        // PINHOLE_SELFTEST_PHOTOS: the photos, by ids; else the newest.
        std::vector<int> photos;
        for (const QString &id : qEnvironmentVariable("PINHOLE_SELFTEST_PHOTOS").split(',', Qt::SkipEmptyParts))
            for (int i = 0; i < library.count(); ++i)
                if (library.at(i).id == id.toUtf8())
                    photos.push_back(i);
        if (photos.empty() && library.count() > 0)
            photos.push_back(library.count() - 1);
        if (photos.empty()) {
            say(QStringLiteral("no photos"));
            QCoreApplication::exit(1);
            return;
        }
        // The grid's frames stepped here, each drawn by a grab, as a window
        // on a workspace not shown draws none of its own: thumbnails asked
        // for, then in and faded in.
        auto settle = [window, grid] {
            for (int i = 0; i < 40; ++i) {
                grid->advance(1.0 / 60);
                window->grabWindow();
            }
        };
        auto gridAt = [grid, settle](int columns, int photo) {
            return [grid, settle, columns, photo] {
                grid->setColumnsNow(columns);
                grid->reveal(photo);
                settle();
            };
        };
        // The sign-in card of a source with no session, out of the way.
        steps->push_back({1500, [window] { press(window, Qt::Key_Escape, {}); }});
        for (const int photo : photos) {
            const QString name = library.at(photo).filename;
            steps->push_back({500, gridAt(3, photo)});
            steps->push_back({1500, settle});
            steps->push_back({0, [grab, name] { grab(name + "-grid-3"); }});
            steps->push_back({0, [grid, photo, &viewer] { viewer.openAt(photo, grid->rectOf(photo)); }});
            steps->push_back({700, [grab, name, &viewer, say] {
                                  say(QStringLiteral("%1: the viewer at its %2").arg(name, viewer.stage()));
                                  grab(name + "-viewer-" + viewer.stage() + "-first");
                              }});
            // The sharpest that comes within 8 s, settled.
            steps->push_back({8000, [grab, name, &viewer, say] {
                                  say(QStringLiteral("%1: the viewer at its %2").arg(name, viewer.stage()));
                                  grab(name + "-viewer-" + viewer.stage());
                                  viewer.close();
                              }});
        }
        const int first = photos.front();
        steps->push_back({800, gridAt(13, first)});
        steps->push_back({1500, settle});
        steps->push_back({0, [grab] { grab("grid-13"); }});
        steps->push_back({0, gridAt(29, first)});
        steps->push_back({2500, settle});
        steps->push_back({0, [grab] { grab("grid-29"); }});
        steps->push_back({0, gridAt(7, first)});
        steps->push_back({800, [window] { press(window, Qt::Key_H, "h"); }});
        steps->push_back({6000, [grab] { grab("mosaic"); }});
        steps->push_back({0, [window] { press(window, Qt::Key_Escape, {}); }});
        (*next)(0);
    });
    poll->start();
}
