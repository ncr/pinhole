#include "keepawake.h"

#include <QEvent>
#include <QGuiApplication>
#include <QQuickWindow>
#include <QtGui/qpa/qplatformwindow_p.h>

#include <cstring>
#include <wayland-client.h>

#include "idle-inhibit-unstable-v1-client-protocol.h"

KeepAwake::KeepAwake(QObject *parent) : QObject(parent) {
    // A frame asked for: on a workspace not shown, none comes, and Qt has
    // the window unexposed (an Expose, looked at below).
    m_ask.setInterval(2000);
    connect(&m_ask, &QTimer::timeout, this, [this] {
        if (!m_window || !m_window->isVisible())
            return;
        // Qt Quick draws one even with nothing changed only so.
        if (auto *quick = qobject_cast<QQuickWindow *>(m_window.data()))
            quick->update();
        else
            m_window->requestUpdate();
    });
}

KeepAwake::~KeepAwake() {
    if (m_inhibitor)
        zwp_idle_inhibitor_v1_destroy(m_inhibitor);
    if (m_manager)
        zwp_idle_inhibit_manager_v1_destroy(m_manager);
    if (m_registry)
        wl_registry_destroy(m_registry);
    if (m_wrapper)
        wl_proxy_wrapper_destroy(m_wrapper);
    if (m_display)
        wl_display_flush(m_display);
    if (m_queue)
        wl_event_queue_destroy(m_queue);
}

void KeepAwake::setWindow(QWindow *window) {
    for (const auto &connection : std::exchange(m_watching, {}))
        disconnect(connection);
    if (m_window)
        m_window->removeEventFilter(this);
    drop();
    m_window = window;
    if (window) {
        // Shown again after it was hidden, its surface is a new one.
        m_watching << connect(window, &QWindow::visibleChanged, this, &KeepAwake::update);
        m_watching << connect(window, &QObject::destroyed, this, &KeepAwake::drop);
        // Seen or not: on a workspace Hyprland is not showing it stays
        // visible, but is no longer exposed.
        window->installEventFilter(this);
    }
    update();
}

bool KeepAwake::eventFilter(QObject *watched, QEvent *event) {
    // Qt has it exposed or not by now; looked at once the event is over.
    if (watched == m_window && event->type() == QEvent::Expose)
        QMetaObject::invokeMethod(this, &KeepAwake::update, Qt::QueuedConnection);
    return false;
}

void KeepAwake::hold(const void *owner, bool on) {
    if (on)
        m_owners.insert(owner);
    else
        m_owners.remove(owner);
    update();
}

void KeepAwake::drop() {
    if (!m_inhibitor)
        return;
    zwp_idle_inhibitor_v1_destroy(m_inhibitor);
    m_inhibitor = nullptr;
    if (m_display)
        wl_display_flush(m_display);
    emit inhibitingChanged();
}

void KeepAwake::update() {
    if (wanted() && m_window) {
        if (!m_ask.isActive())
            m_ask.start();
    } else {
        m_ask.stop();
    }
    const bool shows = m_window && m_window->isVisible() && exposed();
    if (shows != m_shows) {
        m_shows = shows;
        emit showsChanged();
    }
    if (!wanted() || !shows) {
        drop();
        return;
    }
    // The manager, bound the first time it is wanted.
    if (!m_display) {
        auto *wayland = qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();
        if (!wayland || !wayland->display())
            return;
        m_display = wayland->display();
        m_queue = wl_display_create_queue(m_display);
        m_wrapper = static_cast<wl_display *>(wl_proxy_create_wrapper(m_display));
        wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(m_wrapper), m_queue);
        m_registry = wl_display_get_registry(m_wrapper);
        static const wl_registry_listener listener = {&KeepAwake::onGlobal, &KeepAwake::onGlobalRemove};
        wl_registry_add_listener(m_registry, &listener, this);
        wl_display_roundtrip_queue(m_display, m_queue);
    }
    auto *platform = m_window->nativeInterface<QNativeInterface::Private::QWaylandWindow>();
    wl_surface *surface = platform ? platform->surface() : nullptr;
    if (!m_manager || !surface) {
        drop();
        return;
    }
    if (m_inhibitor)
        return;
    // A new surface (the window shown again) needs a new inhibitor.
    m_watching << connect(platform, &QNativeInterface::Private::QWaylandWindow::surfaceDestroyed, this,
                          &KeepAwake::drop, Qt::UniqueConnection);
    m_watching << connect(platform, &QNativeInterface::Private::QWaylandWindow::surfaceCreated, this,
                          &KeepAwake::update, Qt::UniqueConnection);
    m_inhibitor = zwp_idle_inhibit_manager_v1_create_inhibitor(m_manager, surface);
    wl_display_flush(m_display);
    emit inhibitingChanged();
}

void KeepAwake::onGlobal(void *data, wl_registry *registry, uint32_t name, const char *interface, uint32_t) {
    auto *self = static_cast<KeepAwake *>(data);
    if (std::strcmp(interface, zwp_idle_inhibit_manager_v1_interface.name) == 0 && !self->m_manager)
        self->m_manager = static_cast<zwp_idle_inhibit_manager_v1 *>(
            wl_registry_bind(registry, name, &zwp_idle_inhibit_manager_v1_interface, 1));
}

void KeepAwake::onGlobalRemove(void *, wl_registry *, uint32_t) {}
