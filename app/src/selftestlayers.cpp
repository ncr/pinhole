// PINHOLE_SELFTEST=layers:<dir>: plugins' layers over a photo, their
// actions' order and complaints (selftest.h), with the test plugin in
// app/tests/layers among the plugins.

#include "selftest.h"

#include "library.h"
#include "photogrid.h"
#include "viewer.h"

#include <QCoreApplication>
#include <QPointer>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTimer>
#include <qpa/qwindowsysteminterface.h>

#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>

namespace {

void press(QQuickWindow *window, int code, Qt::KeyboardModifiers mods = Qt::NoModifier) {
    QWindowSystemInterface::handleKeyEvent<QWindowSystemInterface::SynchronousDelivery>(window, QEvent::KeyPress, code, mods);
    QWindowSystemInterface::handleKeyEvent<QWindowSystemInterface::SynchronousDelivery>(window, QEvent::KeyRelease, code, mods);
}

QRectF inScene(QQuickItem *item) {
    return item ? item->mapRectToScene(QRectF(0, 0, item->width(), item->height())) : QRectF();
}

QString describe(const QRectF &r) {
    return QStringLiteral("%1,%2 %3x%4").arg(r.x(), 0, 'f', 1).arg(r.y(), 0, 'f', 1).arg(r.width(), 0, 'f', 1).arg(r.height(), 0, 'f', 1);
}

// The first item under `item` whose text is `text` and that shows.
QQuickItem *shownWithText(QQuickItem *item, const QString &text) {
    if (item->isVisible() && item->property("text").toString() == text && item->opacity() > 0)
        return item;
    for (QQuickItem *child : item->childItems())
        if (QQuickItem *found = shownWithText(child, text))
            return found;
    return nullptr;
}

// The complaint saying `text` now (Complaint.qml), if any, among the
// items drawn under `item`.
QQuickItem *saying(QQuickItem *item, const QString &text) {
    if (item->objectName() == QLatin1String("complaint") && item->property("saying").toBool()
        && item->property("text").toString() == text)
        return item;
    for (QQuickItem *child : item->childItems())
        if (QQuickItem *found = saying(child, text))
            return found;
    return nullptr;
}
QQuickItem *saying(QQuickWindow *window, const QString &text) { return saying(window->contentItem(), text); }

} // namespace

void runLayersSelfTest(QQuickWindow *window, PhotoGrid *grid, Viewer &viewer, const Library &library, const QString &folder) {
    auto failures = std::make_shared<int>(0);
    auto check = [failures](bool ok, const QString &what) {
        std::printf("selftest: %s %s\n", ok ? "ok  " : "FAIL", qPrintable(what));
        std::fflush(stdout);
        *failures += ok ? 0 : 1;
    };
    auto grab = [window, folder](const QString &name) {
        if (!folder.isEmpty())
            window->grabWindow().save(QStringLiteral("%1/%2.png").arg(folder, name));
    };
    auto plugin = [window]() -> QObject * {
        QVariant item;
        QMetaObject::invokeMethod(window, "pluginItem", Q_RETURN_ARG(QVariant, item), Q_ARG(QVariant, QStringLiteral("test.layers")));
        return item.value<QObject *>();
    };
    auto made = [plugin] { return plugin() ? plugin()->property("made").toInt() : -1; };
    auto unshown = [plugin] { return plugin() ? plugin()->property("unshown").toInt() : -1; };
    auto layer = [plugin]() -> QQuickItem * { return plugin() ? plugin()->property("last").value<QQuickItem *>() : nullptr; };
    auto frame = [window] { return window->findChild<QQuickItem *>(QStringLiteral("viewerFrame")); };
    // The layer on the photo as it lies now: the frame's rect, to half a pixel.
    auto onPhoto = [frame, layer] {
        const QRectF a = inScene(layer()), b = inScene(frame());
        return !a.isEmpty() && std::abs(a.x() - b.x()) < 0.5 && std::abs(a.y() - b.y()) < 0.5
            && std::abs(a.width() - b.width()) < 0.5 && std::abs(a.height() - b.height()) < 0.5;
    };
    auto steps = std::make_shared<QList<std::pair<int, std::function<void()>>>>();
    auto at = [steps](int ms, std::function<void()> f) { steps->append({ms, std::move(f)}); };
    auto shown = std::make_shared<int>(-1);
    auto old = std::make_shared<QPointer<QQuickItem>>();

    at(0, [=, &viewer, &library] {
        check(plugin() != nullptr, QStringLiteral("the test plugin test.layers is loaded"));
        for (int i = library.count() - 1; i >= 0 && *shown < 0; --i)
            if (!library.at(i).video)
                *shown = i;
        check(made() == 0, QStringLiteral("no layer before a photo is open (%1)").arg(made()));
        viewer.openAt(*shown, grid->rectOf(*shown));
    });
    at(800, [=, &viewer] {
        check(made() == 1, QStringLiteral("one layer made as the photo opened (%1)").arg(made()));
        check(layer() && layer()->property("shown").toBool() && layer()->property("photo").toInt() == viewer.index(),
              QStringLiteral("it is shown, on photo %1").arg(layer() ? layer()->property("photo").toInt() : -1));
        check(onPhoto(), QStringLiteral("it lies on the photo: %1, the photo %2").arg(describe(inScene(layer())), describe(inScene(frame()))));
        grab(QStringLiteral("layers-open"));
        viewer.zoomStep(2);
    });
    at(1400, [=, &viewer] {
        check(viewer.zoom() > 1.5, QStringLiteral("zoomed in, %1×").arg(viewer.zoom(), 0, 'f', 2));
        viewer.panBy(120, 60);
        check(onPhoto(), QStringLiteral("zoomed and panned, it still lies on the photo: %1, the photo %2")
                             .arg(describe(inScene(layer())), describe(inScene(frame()))));
        grab(QStringLiteral("layers-zoomed"));
        check(made() == 1, QStringLiteral("zooming made no other layer (%1)").arg(made()));
        viewer.resetZoom();
    });
    at(3000, [=, &viewer] {
        *old = layer();
        const int before = unshown();
        std::printf("selftest: zoom %.2f, viewer takes the keys: %s\n", viewer.zoom(), viewer.takesInput() ? "yes" : "no");
        press(window, Qt::Key_Left);
        check(!*old || !(*old)->property("shown").toBool(), QStringLiteral("← : the layer is told at once it is no longer shown"));
        check(unshown() == before + 1, QStringLiteral("… once (%1)").arg(unshown() - before));
        Q_UNUSED(viewer);
    });
    at(3500, [=, &viewer] {
        check(old->isNull(), QStringLiteral("the old layer is gone"));
        check(made() == 2 && layer() && layer()->property("photo").toInt() == viewer.index() && layer()->property("shown").toBool(),
              QStringLiteral("a new one is made for photo %1 (%2 made)").arg(viewer.index()).arg(made()));
        check(onPhoto(), QStringLiteral("and lies on it"));
        // Passing quickly: none made for the photos on the way.
        press(window, Qt::Key_Right);
        press(window, Qt::Key_Left);
        press(window, Qt::Key_Right);
    });
    at(4200, [=, &viewer] {
        check(made() == 3 && layer() && layer()->property("photo").toInt() == viewer.index(),
              QStringLiteral("three photos passed quickly: one layer made, for the last (%1 made in all)").arg(made()));
        *old = layer();
        press(window, Qt::Key_Escape);
        check(*old && !(*old)->property("shown").toBool(), QStringLiteral("Esc: told it is no longer shown as the photo shrinks"));
        check(!old->isNull(), QStringLiteral("… and still there, shrinking with it"));
    });
    at(4900, [=, &viewer] {
        check(!viewer.isOpen() && old->isNull(), QStringLiteral("closed: the layer is gone"));
        viewer.openAt(*shown, grid->rectOf(*shown));
    });
    at(5800, [=] { press(window, Qt::Key_F9); });
    at(6200, [=] {
        QQuickItem *said = saying(window, QStringLiteral("Could not test"));
        check(plugin() && plugin()->property("triggered").toInt() == 1, QStringLiteral("F9 presses the viewer's test action"));
        check(said != nullptr, QStringLiteral("F9: the viewer action complains under its button"));
        grab(QStringLiteral("layers-complaint"));
        if (said) {
            QQuickItem *button = shownWithText(window->contentItem(), QStringLiteral("Test"));
            const QRectF b = inScene(button), c = inScene(said);
            check(button && c.top() > b.bottom() && std::abs(c.left() - b.left()) < 1,
                  QStringLiteral("… under it: %1, the button %2").arg(describe(c), describe(b)));
        }
    });
    // One action in three places (API 15): pressed in the viewer, on the
    // photo shown.
    at(6400, [=, &viewer, &library] {
        check(shownWithText(window->contentItem(), QStringLiteral("Everywhere")) != nullptr,
              QStringLiteral("the action of three places has its button in the viewer"));
        press(window, Qt::Key_F8);
        const QString id = QString::fromUtf8(library.at(viewer.index()).id);
        check(plugin()->property("pressedIn").toString() == QLatin1String("viewer") && plugin()->property("photos").toString() == id,
              QStringLiteral("F8 in the viewer: pressed in %1 on %2 (the photo shown is %3)")
                  .arg(plugin()->property("pressedIn").toString(), plugin()->property("photos").toString(), id));
    });
    at(10700, [=] {
        QQuickItem *said = saying(window, QStringLiteral("Could not test"));
        check(said == nullptr, QStringLiteral("four seconds later it is gone"));
        press(window, Qt::Key_Escape);
    });
    at(11500, [=] {
        grid->setSelecting(true);
        grid->selectAll();
    });
    at(12200, [=] {
        QQuickItem *bar = window->findChild<QQuickItem *>(QStringLiteral("selectBar"));
        QQuickItem *copy = window->findChild<QQuickItem *>(QStringLiteral("selectCopyGroup"));
        QQuickItem *before = bar ? shownWithText(bar, QStringLiteral("Before")) : nullptr;
        check(before && copy && inScene(before).right() <= inScene(copy).left() + 1,
              QStringLiteral("order -5 is before Copy in select mode's bar: %1, Copy %2")
                  .arg(describe(inScene(before)), describe(inScene(copy))));
        press(window, Qt::Key_F9);
    });
    at(12600, [=] {
        QQuickItem *bar = window->findChild<QQuickItem *>(QStringLiteral("selectBar"));
        QQuickItem *before = bar ? shownWithText(bar, QStringLiteral("Before")) : nullptr;
        QQuickItem *said = saying(window, QStringLiteral("Nothing before"));
        check(said && before && inScene(said).bottom() < inScene(before).top(),
              QStringLiteral("its complaint is over the bar at the bottom: %1, the button %2")
                  .arg(describe(inScene(said)), describe(inScene(before))));
        grab(QStringLiteral("layers-select"));
        press(window, Qt::Key_F8);
        const QStringList ids = plugin()->property("photos").toString().split(',', Qt::SkipEmptyParts);
        check(plugin()->property("pressedIn").toString() == QLatin1String("selection") && ids.size() == grid->selectionCount(),
              QStringLiteral("F8 in select mode: pressed in %1 on %2 photos (%3 ticked)")
                  .arg(plugin()->property("pressedIn").toString()).arg(ids.size()).arg(grid->selectionCount()));
        grid->setSelecting(false);
    });
    at(13300, [=] {
        check(shownWithText(window->contentItem(), QStringLiteral("Everywhere")) != nullptr,
              QStringLiteral("… and its button on the grid"));
        press(window, Qt::Key_F8);
        check(plugin()->property("pressedIn").toString() == QLatin1String("grid") && plugin()->property("photos").toString().isEmpty(),
              QStringLiteral("F8 on the grid: pressed in %1, on no photos (%2)")
                  .arg(plugin()->property("pressedIn").toString(), plugin()->property("photos").toString()));
    });
    at(13600, [failures] {
        std::printf("selftest: done, %d failed\n", *failures);
        std::fflush(stdout);
        QCoreApplication::exit(*failures ? 1 : 0);
    });

    QTimer::singleShot(2000, window, [window, steps, plugin] {
        // Without the test plugin there is nothing to test.
        if (!plugin()) {
            std::printf("selftest: FAIL the test plugin test.layers is not loaded: put app/tests/layers among the plugins "
                        "(a copy in PINHOLE_BUILTIN_PLUGINS, or in ~/.config/pinhole/plugins, turned on)\n"
                        "selftest: done, 1 failed\n");
            std::fflush(stdout);
            QCoreApplication::exit(1);
            return;
        }
        for (const auto &[ms, f] : *steps)
            QTimer::singleShot(ms, window, f);
    });
}
