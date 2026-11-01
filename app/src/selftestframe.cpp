// PINHOLE_SELFTEST=frame: the Photo frame plugin as it runs (selftest.h).

#include "selftest.h"

#include "plugins.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTimer>
#include <QVector3D>

#include <cstdio>
#include <functional>
#include <memory>

void runFrameSelfTest(QQuickWindow *window) {
    auto failures = std::make_shared<int>(0);
    auto check = [failures](bool ok, const QString &what) {
        std::printf("selftest: %s %s\n", ok ? "ok  " : "FAIL", qPrintable(what));
        std::fflush(stdout);
        if (!ok)
            ++*failures;
    };
    auto finish = [failures] { QCoreApplication::exit(*failures ? 1 : 0); };
    // `ready` asked every 20 ms, then `then`; or after `ms`, a failure.
    auto waitFor = std::make_shared<std::function<void(std::function<bool()>, int, QString, std::function<void()>)>>();
    *waitFor = [window, check, finish](std::function<bool()> ready, int ms, QString what, std::function<void()> then) {
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

    QTimer::singleShot(2500, window, [=] {
        // A plugin's items are the window's items' children.
        std::function<QQuickItem *(QQuickItem *)> find = [&](QQuickItem *item) -> QQuickItem * {
            if (item->objectName() == QLatin1String("photoFrame"))
                return item;
            for (QQuickItem *child : item->childItems())
                if (QQuickItem *found = find(child))
                    return found;
            return nullptr;
        };
        QQuickItem *frame = find(window->contentItem());
        auto *plugins = qobject_cast<Plugins *>(qmlEngine(window)->rootContext()->contextProperty("plugins").value<QObject *>());
        if (!frame || !plugins) {
            check(false, "the Photo frame plugin is loaded");
            return finish();
        }
        KeepAwake *awake = plugins->keepAwake();
        const bool wayland = QGuiApplication::platformName().startsWith(QLatin1String("wayland"));
        auto slide = [frame](const char *which) { return frame->property(which).value<QObject *>(); };

        // Started from the grid, on all the photos.
        QMetaObject::invokeMethod(frame->property("plugin").value<QObject *>(), "begin", Q_ARG(QVariant, QStringLiteral("all")));
        check(frame->property("shown").toBool(), "the frame shows");
        check(awake->wanted(), "it asks for the screen to stay on");
        if (wayland)
            check(awake->inhibiting(), "an idle inhibitor is on the window's surface");

        // On to the next, and held still halfway through the fade: both
        // slides, and the fade, stay where they are.
        (*waitFor)([=] { return slide("current")->property("ready").toBool(); }, 15000, "the first photo comes", [=] {
            QObject *outgoing = slide("current");
            QMetaObject::invokeMethod(frame, "go", Q_ARG(QVariant, 1), Q_ARG(QVariant, false));
            (*waitFor)([=] { return outgoing->property("opacity").toReal() < 0.6; }, 10000, "a fade begins", [=] {
                QMetaObject::invokeMethod(frame, "setPaused", Q_ARG(QVariant, true));
                QObject *incoming = slide("next");
                const qreal opacity = outgoing->property("opacity").toReal();
                const QVector3D in = incoming->property("framed").value<QVector3D>();
                const QVector3D out = outgoing->property("framed").value<QVector3D>();
                QTimer::singleShot(1500, window, [=] {
                    check(qFuzzyCompare(outgoing->property("opacity").toReal(), opacity),
                          QStringLiteral("the fade holds still (%1 → %2)").arg(opacity).arg(outgoing->property("opacity").toReal()));
                    check(incoming->property("framed").value<QVector3D>() == in, "the photo coming holds still");
                    check(outgoing->property("framed").value<QVector3D>() == out, "the photo going holds still");
                    QMetaObject::invokeMethod(frame, "setPaused", Q_ARG(QVariant, false));
                    (*waitFor)([=] { return slide("current") == incoming; }, 3000, "the fade goes on to its end", [=] {
                        check(true, "the fade goes on to its end");
                        check(incoming->property("framed").value<QVector3D>() != in, "the photo moves again");
                        // Stopped: the screen let go at once, the photo fading
                        // out with the frame, and then let go too.
                        QMetaObject::invokeMethod(frame, "stop");
                        check(!awake->wanted() && !awake->inhibiting(), "stopped, the screen may sleep again");
                        QTimer::singleShot(100, window, [=] {
                            check(frame->property("opacity").toReal() > 0.05
                                      && !incoming->property("photoId").toString().isEmpty(),
                                  "the photo fades out, not black");
                            QTimer::singleShot(600, window, [=] {
                                check(incoming->property("photoId").toString().isEmpty()
                                          && outgoing->property("photoId").toString().isEmpty(),
                                      "then the slides let their photos go");
                                finish();
                            });
                        });
                    });
                });
            });
        });
    });
}
