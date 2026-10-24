#pragma once

#include <QObject>

struct wl_display;
struct wl_event_queue;
struct wl_registry;
struct wl_pointer;
struct zwp_pointer_gestures_v1;
struct zwp_pointer_gesture_hold_v1;

// Fingers resting on the touchpad. A Mac stops a coasting scroll the moment
// a finger touches; on Linux the only signal for that is libinput's hold
// gesture, which the compositor passes on through the pointer-gestures
// protocol. Qt binds that protocol for pinch and swipe but not for hold, so
// this binds it on Qt's own Wayland connection, on an event queue of its own
// so Qt's dispatching is never disturbed.
class TouchpadHold : public QObject {
    Q_OBJECT
public:
    explicit TouchpadHold(QObject *parent = nullptr);
    ~TouchpadHold() override;

    bool available() const { return m_hold != nullptr; }
    // Deliver whatever arrived since the last call. Cheap: call it every
    // frame while a touch would matter.
    void poll();

signals:
    // `time` is in milliseconds, on the same clock as wheel event timestamps.
    void began(quint32 time, quint32 fingers);
    void ended(quint32 time, bool cancelled);

private:
    void ensureGesture();
    static void onGlobal(void *data, wl_registry *registry, uint32_t name, const char *interface, uint32_t version);
    static void onGlobalRemove(void *data, wl_registry *registry, uint32_t name);
    static void onBegin(void *data, zwp_pointer_gesture_hold_v1 *hold, uint32_t serial, uint32_t time,
                        struct wl_surface *surface, uint32_t fingers);
    static void onEnd(void *data, zwp_pointer_gesture_hold_v1 *hold, uint32_t serial, uint32_t time, int32_t cancelled);

    wl_display *m_display = nullptr;
    wl_event_queue *m_queue = nullptr;
    wl_display *m_wrapper = nullptr;
    wl_registry *m_registry = nullptr;
    zwp_pointer_gestures_v1 *m_gestures = nullptr;
    zwp_pointer_gesture_hold_v1 *m_hold = nullptr;
};
