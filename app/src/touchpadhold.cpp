#include "touchpadhold.h"

#include <QGuiApplication>

#include <cstring>
#include <wayland-client.h>

#include "pointer-gestures-unstable-v1-client-protocol.h"

TouchpadHold::TouchpadHold(QObject *parent) : QObject(parent) {
    auto *wayland = qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();
    if (!wayland || !wayland->display())
        return;
    m_display = wayland->display();
    m_queue = wl_display_create_queue(m_display);
    // A wrapper of the display on our queue: the registry made from it, and
    // everything bound through that, delivers to our queue only.
    m_wrapper = static_cast<wl_display *>(wl_proxy_create_wrapper(m_display));
    wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(m_wrapper), m_queue);
    m_registry = wl_display_get_registry(m_wrapper);
    static const wl_registry_listener listener = {&TouchpadHold::onGlobal, &TouchpadHold::onGlobalRemove};
    wl_registry_add_listener(m_registry, &listener, this);
    wl_display_roundtrip_queue(m_display, m_queue);
    ensureGesture();
}

TouchpadHold::~TouchpadHold() {
    if (m_hold)
        zwp_pointer_gesture_hold_v1_destroy(m_hold);
    if (m_gestures)
        zwp_pointer_gestures_v1_release(m_gestures);
    if (m_registry)
        wl_registry_destroy(m_registry);
    if (m_wrapper)
        wl_proxy_wrapper_destroy(m_wrapper);
    if (m_display)
        wl_display_flush(m_display);
    if (m_queue)
        wl_event_queue_destroy(m_queue);
}

void TouchpadHold::onGlobal(void *data, wl_registry *registry, uint32_t name, const char *interface,
                            uint32_t version) {
    auto *self = static_cast<TouchpadHold *>(data);
    // Hold arrived in version 3 of the protocol.
    if (std::strcmp(interface, zwp_pointer_gestures_v1_interface.name) == 0 && version >= 3 && !self->m_gestures) {
        self->m_gestures = static_cast<zwp_pointer_gestures_v1 *>(
            wl_registry_bind(registry, name, &zwp_pointer_gestures_v1_interface, 3));
    }
}

void TouchpadHold::onGlobalRemove(void *, wl_registry *, uint32_t) {}

void TouchpadHold::ensureGesture() {
    if (m_hold || !m_gestures)
        return;
    // Qt creates its pointer once the seat says it has one; until then
    // there is nothing to attach to.
    auto *wayland = qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();
    wl_pointer *pointer = wayland ? wayland->pointer() : nullptr;
    if (!pointer)
        return;
    m_hold = zwp_pointer_gestures_v1_get_hold_gesture(m_gestures, pointer);
    static const zwp_pointer_gesture_hold_v1_listener listener = {&TouchpadHold::onBegin, &TouchpadHold::onEnd};
    zwp_pointer_gesture_hold_v1_add_listener(m_hold, &listener, this);
    wl_display_flush(m_display);
}

void TouchpadHold::poll() {
    if (!m_display)
        return;
    ensureGesture();
    // Qt's reader puts our events on our queue; this only hands them out.
    wl_display_dispatch_queue_pending(m_display, m_queue);
}

void TouchpadHold::onBegin(void *data, zwp_pointer_gesture_hold_v1 *, uint32_t, uint32_t time, wl_surface *,
                           uint32_t fingers) {
    emit static_cast<TouchpadHold *>(data)->began(time, fingers);
}

void TouchpadHold::onEnd(void *data, zwp_pointer_gesture_hold_v1 *, uint32_t, uint32_t time, int32_t cancelled) {
    emit static_cast<TouchpadHold *>(data)->ended(time, cancelled != 0);
}
