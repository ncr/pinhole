#include "viewer.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QNativeGestureEvent>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTimer>
#include <QWheelEvent>
#include <qpa/qwindowsysteminterface.h>

#include <cstdio>
#include <memory>

namespace {
void press(QQuickWindow *window, int key) {
    using QWS = QWindowSystemInterface;
    QWS::handleKeyEvent<QWS::SynchronousDelivery>(window, QEvent::KeyPress, key, Qt::NoModifier);
    QWS::handleKeyEvent<QWS::SynchronousDelivery>(window, QEvent::KeyRelease, key, Qt::NoModifier);
}

void click(QQuickWindow *window, QQuickItem *item) {
    using QWS = QWindowSystemInterface;
    const auto at = item->mapToScene(QPointF(item->width(), item->height()) / 2);
    const auto global = window->mapToGlobal(at.toPoint());
    QWS::handleMouseEvent<QWS::SynchronousDelivery>(window, at, global,
        Qt::LeftButton, Qt::LeftButton, QEvent::MouseButtonPress);
    QWS::handleMouseEvent<QWS::SynchronousDelivery>(window, at, global,
        Qt::NoButton, Qt::LeftButton, QEvent::MouseButtonRelease);
}

QQuickItem *named(QQuickItem *root, const QString &name) {
    if (!root) return nullptr;
    if (root->objectName() == name) return root;
    for (auto *child : root->childItems())
        if (auto *found = named(child, name)) return found;
    return nullptr;
}

QQuickItem *groupMarker(QQuickItem *root) {
    if (root->objectName() == QStringLiteral("placesMarker") && root->isVisible() && root->property("count").toInt() == 2)
        return root;
    for (auto *child : root->childItems())
        if (auto *found = groupMarker(child))
            return found;
    return nullptr;
}
}

void runPlacesSelfTest(QQuickWindow *window, Viewer *viewer, const QString &folder) {
    auto *timer = new QTimer(window);
    auto phase = std::make_shared<int>(0);
    auto clock = std::make_shared<QElapsedTimer>();
    clock->start();
    QObject::connect(timer, &QTimer::timeout, window, [=] {
        if (clock->elapsed() > 18000) {
            std::fprintf(stderr, "selftest: places: timed out at step %d\n", *phase);
            if (!folder.isEmpty()) window->grabWindow().save(folder + "/places-timeout.png");
            timer->stop();
            return QCoreApplication::exit(1);
        }
        auto *map = named(window->contentItem(), "photoPlaces");
        auto *viewport = map ? named(map, "placesViewport") : nullptr;
        auto *gallery = map ? named(map, "placesGallery") : nullptr;
        switch (*phase) {
        case 0:
            if (clock->elapsed() < 1200) return;
            press(window, Qt::Key_M);
            break;
        case 1: {
            if (!map || !viewport || !map->isVisible() || !groupMarker(viewport) || clock->elapsed() < 2300) return;
            if (!folder.isEmpty()) window->grabWindow().save(folder + "/places.png");
            // Centered zoom is different from zoom at the pointer. Verify
            // both the zoom and that the longitude under this pointer stays put.
            const double zoom = map->property("zoom").toDouble(), x = map->property("cx").toDouble();
            const auto at = viewport->mapToScene(QPointF(viewport->width() * 0.7, viewport->height() / 2));
            QWheelEvent wheel(at, window->mapToGlobal(at.toPoint()), {}, QPoint(0, 120),
                Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
            QCoreApplication::sendEvent(window, &wheel);
            if (map->property("zoom").toDouble() <= zoom || map->property("cx").toDouble() == x) {
                std::fputs("selftest: places: wheel did not zoom at the pointer\n", stderr);
                return QCoreApplication::exit(1);
            }
            const auto native = [&](Qt::NativeGestureType type, double amount) {
                QNativeGestureEvent gesture(type, QPointingDevice::primaryPointingDevice(), 2,
                    at, at, window->mapToGlobal(at.toPoint()), amount, {}, 1);
                QCoreApplication::sendEvent(window, &gesture);
            };
            const double beforePinch = map->property("zoom").toDouble();
            native(Qt::BeginNativeGesture, 0);
            native(Qt::ZoomNativeGesture, 0.25);
            native(Qt::EndNativeGesture, 0);
            if (map->property("zoom").toDouble() <= beforePinch) {
                std::fputs("selftest: places: touchpad pinch did not zoom\n", stderr);
                return QCoreApplication::exit(1);
            }
            // Restore the overview before clicking the two-photo group.
            QMetaObject::invokeMethod(map, "fitAll");
            break;
        }
        case 2:
            if (!viewport || !groupMarker(viewport)) return;
            click(window, groupMarker(viewport));
            break;
        case 3:
            if (!gallery || gallery->property("count").toInt() != 2) return;
            if (!folder.isEmpty()) window->grabWindow().save(folder + "/places-gallery.png");
            press(window, Qt::Key_Return);
            break;
        case 4:
            if (!viewer->isOpen() || !map || map->isVisible()) return;
            press(window, Qt::Key_Escape);
            break;
        case 5:
            if (viewer->isOpen() || !map || !map->isVisible() || gallery->property("count").toInt() != 2) return;
            press(window, Qt::Key_Escape);
            break;
        case 6:
            if (!gallery || gallery->property("count").toInt() != 0) return;
            press(window, Qt::Key_Escape);
            break;
        case 7:
            if (map) return;
            viewer->openAt(0, {});
            break;
        case 8:
            if (!viewer->isOpen() || !named(window->contentItem(), "photoViewer")) return;
            press(window, Qt::Key_I);
            break;
        case 9: {
            auto *button = named(window->contentItem(), "infoMap");
            if (!button || !button->isVisible()) return;
            press(window, Qt::Key_W);
            break;
        }
        case 10:
            if (!map || !map->isVisible() || !viewer->isOpen() || map->property("zoom").toDouble() != 14) return;
            press(window, Qt::Key_Escape);
            break;
        default:
            if (map || !viewer->isOpen()) return;
            std::puts("selftest: places: M, grouped photos, wheel and pinch zoom, viewer return, Escape and Info W: PASS");
            timer->stop();
            return QCoreApplication::quit();
        }
        ++*phase;
    });
    timer->start(180);
}
