#include "panzoom.h"
#include "viewer.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QNativeGestureEvent>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTimer>
#include <QWheelEvent>
#include <private/qhighdpiscaling_p.h>
#include <private/qquickwindow_p.h>
#include <qpa/qwindowsysteminterface.h>

#include <cstdio>
#include <functional>
#include <memory>

namespace {
using QWS = QWindowSystemInterface;
using Delivery = QWS::SynchronousDelivery;

void press(QQuickWindow *window, int key, Qt::KeyboardModifiers mods = Qt::NoModifier) {
    QWS::handleKeyEvent<Delivery>(window, QEvent::KeyPress, key, mods);
    QWS::handleKeyEvent<Delivery>(window, QEvent::KeyRelease, key, mods);
}

QPointF center(QQuickItem *item) {
    return item->mapToScene(QPointF(item->width(), item->height()) / 2);
}

void pointer(QQuickWindow *window, const QPointingDevice *device, QPointF at,
             Qt::MouseButtons state, Qt::MouseButton button, QEvent::Type type) {
    QWS::handleMouseEvent<Delivery>(window, device, QHighDpi::toNativePixels(at, window),
        QHighDpi::toNativeGlobalPosition(window->mapToGlobal(at), window), state, button, type);
}

void click(QQuickWindow *window, const QPointingDevice *device, QQuickItem *item,
           Qt::MouseButton button = Qt::LeftButton) {
    const auto at = center(item);
    pointer(window, device, at, button, button, QEvent::MouseButtonPress);
    pointer(window, device, at, Qt::NoButton, button, QEvent::MouseButtonRelease);
}

QQuickItem *findItem(QQuickItem *root, const std::function<bool(QQuickItem *)> &matches) {
    if (!root || !root->isVisible())
        return nullptr;
    if (matches(root))
        return root;
    for (auto *child : root->childItems())
        if (auto *found = findItem(child, matches))
            return found;
    return nullptr;
}

QQuickItem *named(QQuickItem *root, const QString &name) {
    return findItem(root, [&](QQuickItem *item) { return item->objectName() == name; });
}

QQuickItem *groupMarker(QQuickItem *root) {
    return findItem(root, [](QQuickItem *item) {
        return item->objectName() == "placesMarker" && item->property("count").toInt() == 2;
    });
}

int markers(QQuickItem *root) {
    int count = 0;
    std::function<void(QQuickItem *)> look = [&](QQuickItem *item) {
        if (!item || !item->isVisible())
            return;
        if (item->objectName() == "placesMarker")
            ++count;
        for (auto *child : item->childItems())
            look(child);
    };
    look(root);
    return count;
}

QQuickItem *mapAction(QQuickItem *root) {
    return findItem(root, [](QQuickItem *item) {
        if (item->property("key").toString() == "M" && item->metaObject()->indexOfSignal("triggered()") >= 0)
            return true;
        const auto text = item->property("text").toString();
        return item->objectName().startsWith("menuOption_") && (text == "Map" || text == "Mapa");
    });
}

QQuickItem *viewerClose(QQuickItem *root) {
    return findItem(named(root, "photoViewer"), [](QQuickItem *item) {
        return item->property("key").toString() == "Escape" && item->metaObject()->indexOfSignal("triggered()") >= 0;
    });
}

void wheel(QQuickWindow *window, const QPointingDevice *device, QPointF at,
           QPoint pixels, QPoint angle, Qt::ScrollPhase phase) {
    QWheelEvent event(at, window->mapToGlobal(at.toPoint()), pixels, angle, Qt::NoButton,
                      Qt::NoModifier, phase, false, Qt::MouseEventNotSynthesized, device);
    QCoreApplication::sendEvent(window, &event);
}

void pinch(QQuickWindow *window, const QPointingDevice *device, QPointF at) {
    const auto send = [&](Qt::NativeGestureType type, double amount) {
        QNativeGestureEvent event(type, device, 2, at, at, window->mapToGlobal(at.toPoint()), amount, {}, 1);
        QCoreApplication::sendEvent(window, &event);
    };
    send(Qt::BeginNativeGesture, 0);
    send(Qt::ZoomNativeGesture, 0.25);
    send(Qt::EndNativeGesture, 0);
}
}

void runPlacesSelfTest(QQuickWindow *window, Viewer *viewer, const QString &folder) {
    struct Run {
        int input = 0;
        int phase = 0;
        double previous = 0;
        double before = 0;
        // Zoomed in by the wheel, which glides there over a few frames:
        // waited for before Fit all.
        bool gliding = false;
        QElapsedTimer clock;
    };
    const auto run = std::make_shared<Run>();
    run->clock.start();
    auto *timer = new QTimer(window);
    auto *touchpad = new QPointingDevice("Test touchpad", 451, QInputDevice::DeviceType::TouchPad,
        QPointingDevice::PointerType::Finger, QInputDevice::Capability::Position | QInputDevice::Capability::Scroll,
        2, 3, QString(), {}, window);
    QWS::registerInputDevice(touchpad);
    QObject::connect(timer, &QTimer::timeout, window, [=] {
        const char *input = run->input == 0 ? "keyboard" : run->input == 1 ? "mouse" : "touchpad";
        const auto fail = [&](const char *why) {
            std::fprintf(stderr, "selftest: places (%s), step %d: %s\n", input, run->phase, why);
            if (!folder.isEmpty()) window->grabWindow().save(folder + "/places-timeout.png");
            timer->stop();
            QCoreApplication::exit(1);
        };
        if (run->clock.elapsed() > 14000)
            return fail("timed out");
        // A tiled window on the hidden workspace may not receive frame callbacks.
        // Settle pending Quick layouts before locating the next control.
        QQuickWindowPrivate::get(window)->polishItems();
        const bool keys = run->input == 0;
        const auto *device = run->input == 2 ? touchpad : QPointingDevice::primaryPointingDevice();
        auto *root = window->contentItem();
        auto *map = named(root, "photoPlaces");
        auto *viewport = named(map, "placesViewport");
        auto *drawer = named(map, "placesDrawer");
        auto *gallery = named(drawer, "placesGallery");
        auto *group = groupMarker(viewport);
        const auto zoom = [&] { return map->property("zoom").toDouble(); };
        const auto longitude = [&] { return map->property("cx").toDouble(); };
        switch (run->phase) {
        case 0:
            if (run->clock.elapsed() < 950) return;
            if (keys) {
                press(window, Qt::Key_M);
            } else if (auto *button = mapAction(root)) {
                click(window, device, button);
            } else {
                auto *more = named(root, "gridMore");
                auto *opener = named(more, "moreButton");
                if (opener && !more->property("open").toBool())
                    click(window, device, opener);
                return;
            }
            break;
        case 1: {
            // Zoomed in, Warsaw's two may be apart: waited for without them.
            if (run->gliding) {
                // Its frames, as the window would draw them (one on a hidden
                // workspace may get none).
                if (auto *pan = qobject_cast<PanZoom *>(findItem(map, [](QQuickItem *item) { return qobject_cast<PanZoom *>(item); })))
                    for (int frame = 0; frame < 12 && pan->advance(1 / 60.0);)
                        ++frame;
                if (zoom() <= run->previous) return;
                run->gliding = false;
                if (keys)
                    press(window, Qt::Key_Home);
                else
                    click(window, device, named(map, "placesFit"));
                if (std::abs(zoom() - run->before) > 0.01)
                    return fail("Fit all did not restore the overview");
                break;
            }
            if (!viewport || !group) return;
            const auto at = viewport->mapToScene(QPointF(viewport->width() * 0.65, viewport->height() / 2));
            const double before = longitude(), beforeZoom = zoom();
            if (keys) {
                press(window, Qt::Key_Right, Qt::ShiftModifier);
            } else if (run->input == 1) {
                pointer(window, device, at, Qt::LeftButton, Qt::LeftButton, QEvent::MouseButtonPress);
                pointer(window, device, at + QPointF(28, 0), Qt::LeftButton, Qt::NoButton, QEvent::MouseMove);
                pointer(window, device, at + QPointF(58, 0), Qt::LeftButton, Qt::NoButton, QEvent::MouseMove);
                pointer(window, device, at + QPointF(58, 0), Qt::NoButton, Qt::LeftButton, QEvent::MouseButtonRelease);
            } else {
                wheel(window, device, at, {}, {}, Qt::ScrollBegin);
                wheel(window, device, at, QPoint(46, 12), {}, Qt::ScrollUpdate);
                wheel(window, device, at, {}, {}, Qt::ScrollEnd);
            }
            if (longitude() == before || zoom() != beforeZoom)
                return fail("pan did not move the map at the same zoom");
            if (keys)
                press(window, Qt::Key_Plus);
            else if (run->input == 1)
                // Wayland can report the seat's touchpad device for a real wheel notch.
                wheel(window, touchpad, at, QPoint(0, 15), QPoint(0, 120), Qt::NoScrollPhase);
            else
                pinch(window, device, at);
            // The keys and a pinch zoom at once, the wheel glides: either
            // way, in within a few frames.
            run->previous = beforeZoom;
            run->before = beforeZoom;
            run->gliding = true;
            return;
        }
        case 2:
            // Both places, Warsaw's two and Lisbon, once the groups are made
            // again for the view Fit all left (a moment after).
            if (!group || markers(viewport) < 2) return;
            if (!folder.isEmpty() && run->input == 0)
                window->grabWindow().save(folder + "/places.png");
            if (keys) {
                // Move from Warsaw to Lisbon and back, or straight to Warsaw
                // if it was the other marker that initially had the focus.
                if (map->property("keyboardPlace").toString() == "b") {
                    press(window, Qt::Key_Left);
                    if (map->property("keyboardPlace").toString() != "c")
                        return fail("Left did not select the marker to the west");
                }
                press(window, Qt::Key_Right);
                if (map->property("keyboardPlace").toString() != "b")
                    return fail("Right did not select the marker to the east");
                press(window, Qt::Key_Return);
            } else {
                click(window, device, group);
                if (!named(map, "placesDrawer"))
                    return fail("click did not open the group immediately");
            }
            break;
        case 3:
            if (!gallery || gallery->property("count").toInt() != 2) return;
            if (!folder.isEmpty() && run->input == 0)
                window->grabWindow().save(folder + "/places-gallery.png");
            if (keys) {
                press(window, Qt::Key_Right);
                if (map->property("cursor").toInt() != 1)
                    return fail("Right did not advance in the gallery");
                press(window, Qt::Key_Space);
            } else if (auto *photo = named(gallery, "photoTile_1")) {
                click(window, device, photo);
            } else {
                return;
            }
            break;
        case 4:
            if (!viewer->isOpen() || map) return;
            // Back to the map: M as it came, Esc, or the viewer's ✕.
            if (keys)
                press(window, Qt::Key_M);
            else if (run->input == 2)
                press(window, Qt::Key_Escape);
            else if (auto *button = viewerClose(root))
                click(window, device, button);
            else
                return;
            break;
        case 5:
            if (viewer->isOpen() || !gallery || map->property("cursor").toInt() != 1) return;
            run->previous = zoom();
            if (keys)
                press(window, Qt::Key_Z);
            else
                click(window, device, named(drawer, "placesZoomGroup"));
            break;
        case 6:
            if (!drawer || zoom() <= run->previous)
                return fail("the group did not zoom in after returning from a photo");
            if (keys)
                press(window, Qt::Key_Escape);
            else if (run->input == 1)
                click(window, device, viewport, Qt::BackButton);
            else
                click(window, device, named(drawer, "placesHideGallery"));
            break;
        case 7:
            if (!map || drawer) return;
            if (keys) {
                press(window, Qt::Key_Home);
                press(window, Qt::Key_G);
            } else {
                click(window, device, named(map, "placesFit"));
                click(window, device, named(map, "placesArea"));
            }
            break;
        case 8:
            if (!drawer || !map->property("areaOpen").toBool()) return;
            if (keys) {
                press(window, Qt::Key_G);
                if (named(map, "placesDrawer"))
                    return fail("G did not close Photos in view");
                press(window, Qt::Key_M);
            } else {
                click(window, device, named(map, "placesArea"));
                if (named(map, "placesDrawer"))
                    return fail("Photos in view did not toggle");
                click(window, device, named(map, "panelClose"));
            }
            break;
        case 9:
            if (map || viewer->isOpen()) return;
            std::printf("selftest: places (%s): open, pan, zoom, browse a group, return, area photos and close: PASS\n", input);
            if (++run->input < 3) {
                run->phase = 0;
                run->clock.restart();
                return;
            }
            // M on a photo, Info shown or not, with a time of its own as
            // each input's run has.
            run->clock.restart();
            viewer->openAt(0, {});
            break;
        case 10:
            if (!viewer->isOpen() || !named(root, "photoViewer")) return;
            if (named(root, "infoPanel"))
                return fail("Info was open before I");
            press(window, Qt::Key_M);
            break;
        case 11: {
            // Its place, read from the index: Info is not open to know it.
            if (!map || !viewer->isOpen()) return;
            const auto focus = map->property("focusPoint").toMap();
            if (zoom() != 14 || focus.value("id").toString().isEmpty())
                return;
            press(window, Qt::Key_M);
            break;
        }
        case 12:
            if (map || !viewer->isOpen()) return;
            std::puts("selftest: places: M on a photo without Info: PASS");
            press(window, Qt::Key_I);
            break;
        case 13:
            if (!named(root, "infoPanel")) return;
            press(window, Qt::Key_M);
            break;
        case 14:
            if (!map || !viewer->isOpen() || zoom() != 14) return;
            press(window, Qt::Key_M);
            break;
        case 15: {
            if (map || !viewer->isOpen()) return;
            std::puts("selftest: places: Info M and return to the photo: PASS");
            press(window, Qt::Key_I);
            press(window, Qt::Key_Escape);
            // Warsaw's two ticked (the oldest: Lisbon, the newest, is last).
            auto *grid = named(root, "grid");
            grid->setProperty("selecting", true);
            for (int at : {0, 1}) {
                QMetaObject::invokeMethod(grid, "putCursorOn", Q_ARG(int, at));
                QMetaObject::invokeMethod(grid, "tickCursor");
            }
            break;
        }
        case 16:
            if (viewer->isOpen() || !named(root, "selectBar")) return;
            press(window, Qt::Key_M);
            break;
        case 17: {
            // Those two in view, close: not the whole world's three.
            if (!map) return;
            const double warsaw = (21.015 + 180) / 360;
            if (zoom() < 10 || std::abs(longitude() - warsaw) > 0.01) return;
            if (!folder.isEmpty())
                window->grabWindow().save(folder + "/places-selection.png");
            press(window, Qt::Key_M);
            break;
        }
        default:
            if (map) return;
            std::puts("selftest: places: M on photos ticked: PASS");
            timer->stop();
            return QCoreApplication::quit();
        }
        ++run->phase;
    });
    timer->start(180);
}
