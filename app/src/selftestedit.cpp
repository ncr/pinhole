// PINHOLE_SELFTEST=edit:<dir>: Turn and crop (plugins/edit) as it runs
// (selftest.h). A photo opened, turned a quarter (its shape follows),
// cropped in the plugin's view and kept; pictures of each in <dir>.

#include "selftest.h"

#include "library.h"
#include "viewer.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTimer>
#include <qpa/qwindowsysteminterface.h>

#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>

void runEditSelfTest(QQuickWindow *window, const Library &library, Viewer &viewer, const QString &dir) {
    auto failures = std::make_shared<int>(0);
    auto check = [failures](bool ok, const QString &what) {
        std::printf("selftest: %s %s\n", ok ? "ok  " : "FAIL", qPrintable(what));
        std::fflush(stdout);
        if (!ok)
            ++*failures;
    };
    auto finish = [failures] { QCoreApplication::exit(*failures ? 1 : 0); };
    auto waitFor = [window, check, finish](std::function<bool()> ready, int ms, QString what, std::function<void()> then) {
        auto clock = std::make_shared<QElapsedTimer>();
        clock->start();
        auto *timer = new QTimer(window);
        timer->setInterval(20);
        QObject::connect(timer, &QTimer::timeout, window, [=] {
            if (ready()) {
                timer->deleteLater();
                then();
            } else if (clock->elapsed() > ms) {
                timer->deleteLater();
                check(false, what + " (timed out)");
                finish();
            }
        });
        timer->start();
    };
    auto shoot = [window, dir](const QString &name) {
        if (!dir.isEmpty())
            window->grabWindow().save(dir + '/' + name + ".png");
    };
    // Kept alive by the closures that use it, which outlive this call.
    auto search = std::make_shared<std::function<QQuickItem *(QQuickItem *, const QString &)>>();
    *search = [weak = std::weak_ptr(search)](QQuickItem *item, const QString &name) -> QQuickItem * {
        if (item->objectName() == name)
            return item;
        for (QQuickItem *child : item->childItems())
            if (QQuickItem *found = (*weak.lock())(child, name))
                return found;
        return nullptr;
    };
    auto find = [search](QQuickItem *item, const QString &name) { return (*search)(item, name); };

    QTimer::singleShot(2500, window, [=, &library, &viewer] {
        QVariant instance;
        QMetaObject::invokeMethod(window, "pluginItem", Q_RETURN_ARG(QVariant, instance),
                                  Q_ARG(QVariant, QStringLiteral("pinhole.edit")));
        QObject *plugin = instance.value<QObject *>();
        if (!plugin) {
            check(false, "the Turn and crop plugin is loaded");
            return finish();
        }
        // A photo wider than high, or any.
        int at = -1;
        for (int i = 0; i < library.count() && at < 0; ++i)
            if (!library.at(i).video && library.at(i).thumbW > library.at(i).thumbH)
                at = i;
        if (at < 0)
            at = 0;
        const QByteArray id = library.at(at).id;
        const QSize before(library.at(at).thumbW, library.at(at).thumbH);
        auto position = [&library, id] {
            for (int i = 0; i < library.count(); ++i)
                if (library.at(i).id == id)
                    return i;
            return -1;
        };
        viewer.openAt(at, {});
        waitFor([&viewer] { return viewer.isOpen(); }, 5000, "the photo opens", [=, &library, &viewer] {
            shoot("open");
            QMetaObject::invokeMethod(plugin, "turn", Q_ARG(QVariant, QVariantList{QString::fromUtf8(id)}), Q_ARG(QVariant, 1));
            waitFor([=, &library] { const int i = position(); return i >= 0 && library.at(i).adjust.turn == 1; }, 5000,
                    "R turns it, kept and read into the library", [=, &library, &viewer] {
                const int i = position();
                check(QSize(library.at(i).thumbW, library.at(i).thumbH) == before.transposed(),
                      QStringLiteral("turned, it is %1x%2 (was %3x%4)").arg(library.at(i).thumbW).arg(library.at(i).thumbH)
                          .arg(before.width()).arg(before.height()));
                check(std::abs(viewer.aspect() - double(before.height()) / before.width()) < 0.02,
                      QStringLiteral("the viewer's shape follows: %1").arg(viewer.aspect()));
                check(viewer.thumbnail().toString().contains(QLatin1String("adjust=1")), "its picture is asked for turned");
                QTimer::singleShot(800, window, [=, &library, &viewer] {
                    shoot("turned");
                    QQuickItem *view = find(window->contentItem(), QStringLiteral("cropping"));
                    if (!view) {
                        check(false, "the crop view is there");
                        return finish();
                    }
                    QMetaObject::invokeMethod(view, "open", Q_ARG(QVariant, QString::fromUtf8(id)));
                    waitFor([=] { return find(window->contentItem(), QStringLiteral("cropView")) != nullptr; }, 5000,
                            "K opens the crop view", [=, &library, &viewer] {
                        QQuickItem *cropView = find(window->contentItem(), QStringLiteral("cropView"));
                        check(cropView->property("turn").toInt() == 1, "it shows the photo turned");
                        // The middle half kept.
                        cropView->setProperty("bx", 0.25);
                        cropView->setProperty("by", 0.25);
                        cropView->setProperty("bw", 0.5);
                        cropView->setProperty("bh", 0.5);
                        QTimer::singleShot(800, window, [=, &library, &viewer] {
                            shoot("crop");
                            QMetaObject::invokeMethod(cropView, "keep");
                            waitFor([=, &library] {
                                const int i = position();
                                return i >= 0 && std::abs(library.at(i).adjust.crop.width() - 0.5) < 1e-3;
                            }, 5000, "⏎ keeps the crop", [=, &library, &viewer] {
                                const int i = position();
                                check(library.at(i).adjust.turn == 1 && std::abs(library.at(i).adjust.crop.x() - 0.25) < 1e-3,
                                      "kept: turned, the middle half");
                                check(std::abs(library.at(i).thumbW - before.height() / 2) <= 1,
                                      QStringLiteral("its size is the part kept: %1x%2").arg(library.at(i).thumbW).arg(library.at(i).thumbH));
                                QTimer::singleShot(800, window, [=, &library, &viewer] {
                                    shoot("cropped");
                                    // ⇧ K: as it was taken again.
                                    QMetaObject::invokeMethod(plugin, "original", Q_ARG(QVariant, QVariantList{QString::fromUtf8(id)}));
                                    waitFor([=, &library] { const int i = position(); return i >= 0 && library.at(i).adjust.isIdentity(); },
                                            5000, "Original takes it back", [=, &library, &viewer] {
                                        const int i = position();
                                        check(QSize(library.at(i).thumbW, library.at(i).thumbH) == before, "as taken, its size as before");
                                        viewer.close();
                                        QTimer::singleShot(1500, window, [=] {
                                            shoot("grid");
                                            finish();
                                        });
                                    });
                                });
                            });
                        });
                    });
                });
            });
        });
    });
}

// PINHOLE_SELFTEST=turn:<dir>: R on the photo PINHOLE_SELFTEST_PHOTO (an
// id), as the person presses it, in a library whose source keeps edits
// (iCloud): each turn drawn at once (how long it took said), two in a row
// sent as one edit once left alone and read back from the source; then ⇧ R
// twice, the photo as it was again. Pictures of each in <dir>.
void runTurnSelfTest(QQuickWindow *window, const Library &library, Viewer &viewer, const QString &dir) {
    auto failures = std::make_shared<int>(0);
    auto check = [failures](bool ok, const QString &what) {
        std::printf("selftest: %s %s\n", ok ? "ok  " : "FAIL", qPrintable(what));
        std::fflush(stdout);
        if (!ok)
            ++*failures;
    };
    auto finish = [failures] { QCoreApplication::exit(*failures ? 1 : 0); };
    auto waitFor = [window, check, finish](std::function<bool()> ready, int ms, QString what, std::function<void()> then) {
        auto clock = std::make_shared<QElapsedTimer>();
        clock->start();
        auto *timer = new QTimer(window);
        timer->setInterval(20);
        QObject::connect(timer, &QTimer::timeout, window, [=] {
            if (ready()) {
                timer->deleteLater();
                std::printf("selftest: %s after %lld ms\n", qPrintable(what), clock->elapsed());
                then();
            } else if (clock->elapsed() > ms) {
                timer->deleteLater();
                check(false, what + " (timed out)");
                finish();
            }
        });
        timer->start();
    };
    auto shoot = [window, dir](const QString &name) {
        if (!dir.isEmpty())
            window->grabWindow().save(dir + '/' + name + ".png");
    };
    const QByteArray id = qgetenv("PINHOLE_SELFTEST_PHOTO");
    auto position = [&library, id] {
        for (int i = 0; i < library.count(); ++i)
            if (library.at(i).id == id)
                return i;
        return -1;
    };
    // R (or ⇧ R) pressed: how long until the library draws it so, and
    // until the next frame is shown.
    auto press = [window, &library, position](bool back) {
        const int at = position();
        const Adjustment before = at >= 0 ? library.at(at).adjust : Adjustment();
        QElapsedTimer clock;
        clock.start();
        const Qt::KeyboardModifiers modifiers = back ? Qt::ShiftModifier : Qt::NoModifier;
        QWindowSystemInterface::handleKeyEvent<QWindowSystemInterface::SynchronousDelivery>(window, QEvent::KeyPress, Qt::Key_R, modifiers, back ? "R" : "r");
        QWindowSystemInterface::handleKeyEvent<QWindowSystemInterface::SynchronousDelivery>(window, QEvent::KeyRelease, Qt::Key_R, modifiers, back ? "R" : "r");
        const qint64 taken = clock.elapsed();
        const bool changed = at >= 0 && library.at(at).adjust != before;
        auto frame = std::make_shared<QMetaObject::Connection>();
        *frame = QObject::connect(window, &QQuickWindow::frameSwapped, window, [frame, clock, back] {
            QObject::disconnect(*frame);
            std::printf("selftest: %s shown on the next frame, %lld ms after the key\n", back ? "⇧ R" : "R", clock.elapsed());
            std::fflush(stdout);
        });
        window->update();
        return std::pair(changed, taken);
    };
    QTimer::singleShot(4000, window, [=, &library, &viewer] {
        const int at = position();
        if (at < 0) {
            check(false, "PINHOLE_SELFTEST_PHOTO is in the library");
            return finish();
        }
        check(library.at(at).sourceEdit.isIdentity(), "the photo has no edit to begin with");
        const QSize before(library.at(at).thumbW, library.at(at).thumbH);
        viewer.openAt(at, {});
        waitFor([&viewer] { return viewer.isOpen(); }, 5000, "the photo opens", [=, &library, &viewer] {
            QTimer::singleShot(1500, window, [=, &library, &viewer] {
                shoot("open");
                auto [changed, ms] = press(false);
                check(changed, QStringLiteral("R turns it in the library at once (%1 ms)").arg(ms));
                const int i = position();
                check(QSize(library.at(i).thumbW, library.at(i).thumbH) == before.transposed(), "turned, its shape follows");
                check(std::abs(viewer.aspect() - double(before.height()) / before.width()) < 0.02, "the viewer's shape follows");
                QTimer::singleShot(300, window, [=, &library, &viewer] {
                    shoot("turned1");
                    auto [changed, ms] = press(false);
                    check(changed && library.at(position()).adjust.turn == 2, QStringLiteral("R again, at once (%1 ms)").arg(ms));
                    // Sent once left alone, as one edit, and read back.
                    waitFor([=, &library] {
                        const int i = position();
                        return i >= 0 && library.at(i).sourceEdit.turn == 2 && library.at(i).adjust.isIdentity();
                    }, 90000, "iCloud has it turned twice, its files drawn as they are", [=, &library, &viewer] {
                        const int i = position();
                        check(QSize(library.at(i).thumbW, library.at(i).thumbH) == before, "upside down: its shape as before");
                        QTimer::singleShot(1500, window, [=, &library, &viewer] {
                            shoot("edited");
                            auto [changed, ms] = press(true);
                            check(changed, QStringLiteral("⇧ R turns it back at once (%1 ms)").arg(ms));
                            QTimer::singleShot(300, window, [=, &library, &viewer] {
                                auto [changed, ms] = press(true);
                                check(changed, QStringLiteral("⇧ R again, at once (%1 ms)").arg(ms));
                                waitFor([=, &library] {
                                    const int i = position();
                                    return i >= 0 && library.at(i).sourceEdit.isIdentity() && library.at(i).adjust.isIdentity();
                                }, 90000, "iCloud has it as taken again", [=, &viewer] {
                                    QTimer::singleShot(1500, window, [=, &viewer] {
                                        shoot("original");
                                        viewer.close();
                                        QTimer::singleShot(500, window, finish);
                                    });
                                });
                            });
                        });
                    });
                });
            });
        });
    });
}
