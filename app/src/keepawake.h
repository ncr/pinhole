#pragma once

#include <QObject>
#include <QPointer>
#include <QSet>
#include <QTimer>
#include <QWindow>

struct wl_display;
struct wl_event_queue;
struct wl_registry;
struct zwp_idle_inhibit_manager_v1;
struct zwp_idle_inhibitor_v1;

// The screen kept on while something asks for it (a plugin's
// pinhole.keepAwake: a photo frame left running): an idle inhibitor on the
// window's surface, as a video player holds, so neither the screensaver nor
// the lock comes while the window shows: visible, and exposed. Hyprland
// would keep the screen on for a window on a workspace it does not show,
// which Qt still has visible; it is not exposed there once its frames stop
// coming (Qt Wayland's frame callback timeout). That timeout starts only
// with a frame asked for, which a still window (a frame held still) never
// asks: while the screen is wanted on, one is asked every two seconds, so a
// window no longer seen is found so. Wayland's idle-inhibit protocol,
// bound on Qt's connection on an event queue of its own, as TouchpadHold
// does. The compositor drops an inhibitor with its connection, so a Pinhole
// that crashes or is killed leaves nothing holding the screen on.
class KeepAwake : public QObject {
    Q_OBJECT
public:
    explicit KeepAwake(QObject *parent = nullptr);
    ~KeepAwake() override;

    // The window whose surface inhibits.
    void setWindow(QWindow *window);
    // `owner` wants the screen kept on, or no longer does: it is kept on
    // while any does.
    void hold(const void *owner, bool on);
    // Something asks for it.
    bool wanted() const { return !m_owners.isEmpty(); }
    // An inhibitor is there: asked for, on Wayland, with the window shown.
    bool inhibiting() const { return m_inhibitor != nullptr; }
    // The window shows: visible and exposed, as last looked at.
    bool shows() const { return m_shows; }
    // How often a frame is asked of the window while the screen is wanted
    // on (a test's sooner).
    void setAskEvery(int ms) { m_ask.setInterval(ms); }

signals:
    void inhibitingChanged();
    void showsChanged();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    // Whether the window is exposed (a test has it say otherwise).
    virtual bool exposed() const { return m_window->isExposed(); }

private:
    void update();
    void drop();
    static void onGlobal(void *data, wl_registry *registry, uint32_t name, const char *interface, uint32_t version);
    static void onGlobalRemove(void *data, wl_registry *registry, uint32_t name);

    QSet<const void *> m_owners;
    QPointer<QWindow> m_window;
    bool m_shows = false;
    QTimer m_ask;
    QList<QMetaObject::Connection> m_watching;
    wl_display *m_display = nullptr;
    wl_event_queue *m_queue = nullptr;
    wl_display *m_wrapper = nullptr;
    wl_registry *m_registry = nullptr;
    zwp_idle_inhibit_manager_v1 *m_manager = nullptr;
    zwp_idle_inhibitor_v1 *m_inhibitor = nullptr;
};
