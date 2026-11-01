#include "zoomer.h"

#include <QCoreApplication>
#include <QNativeGestureEvent>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace {
// As the grid's (photogrid.cpp): the settle, a pinch's speed carried on,
// the hysteresis at the point halfway, and the sideways wheel's step, ease,
// rest and dead turn.
constexpr double kSettleSeconds = 0.3;
constexpr double kPinchProjection = 0.12;
constexpr double kZoomHysteresis = 0.08;
constexpr double kWheelZoomStep = 1.25;
constexpr double kWheelZoomEase = 0.06;
constexpr double kWheelZoomRest = 0.2;
constexpr double kWheelZoomDead = 0.3;

double easeOut(double t) { return 1 - std::pow(1 - std::clamp(t, 0.0, 1.0), 3); }

void registerZoomer() { qmlRegisterType<Zoomer>("Pinhole", 1, 0, "Zoomer"); }
} // namespace
Q_COREAPP_STARTUP_FUNCTION(registerZoomer)

Zoomer::Zoomer(QQuickItem *parent) : QQuickItem(parent) {
    // The wheel and pinches only: presses and touches go on to what it holds.
    setAcceptedMouseButtons(Qt::NoButton);
    m_clock.start();
}

Zoomer::~Zoomer() = default;

void Zoomer::setLevels(const QList<qreal> &levels) {
    if (levels == m_levels)
        return;
    m_levels = levels;
    std::sort(m_levels.begin(), m_levels.end());
    m_level = std::clamp(m_level, 0, std::max(0, int(m_levels.size()) - 1));
    m_shown = size();
    emit levelsChanged();
    emit levelChanged();
    show(m_shown);
}

void Zoomer::setLevel(int level) {
    // Before its levels (QML sets them in any order), as it is asked.
    level = m_levels.isEmpty() ? std::max(0, level) : std::clamp(level, 0, int(m_levels.size()) - 1);
    const bool was = zooming();
    m_pinching = m_settling = m_wheeling = false;
    if (level != m_level) {
        m_level = level;
        emit levelChanged();
    }
    show(size());
    if (was)
        emit zoomingChanged();
}

qreal Zoomer::size() const { return m_levels.isEmpty() ? 1 : m_levels[m_level]; }

double Zoomer::seconds() const { return m_clock.nsecsElapsed() / 1e9; }

void Zoomer::show(double shown) {
    m_shown = shown;
    const double factor = size() > 0 ? shown / size() : 1;
    if (std::abs(factor - m_factor) > 1e-12) {
        m_factor = factor;
        emit factorChanged();
    }
}

int Zoomer::nearest(double shown) const {
    int best = 0;
    for (int i = 1; i < int(m_levels.size()); ++i)
        if (std::abs(std::log(shown / m_levels[i])) < std::abs(std::log(shown / m_levels[best])))
            best = i;
    return best;
}

void Zoomer::begin(QPointF at) {
    const bool was = zooming();
    m_settling = m_wheeling = false;
    m_pinching = true;
    m_origin = at;
    m_startShown = m_shown > 0 ? m_shown : size();
    m_pinchScale = 1;
    m_samples.clear();
    emit factorChanged();
    if (!was)
        emit zoomingChanged();
}

void Zoomer::zoomTo(double shown, double now) {
    if (m_levels.isEmpty())
        return;
    // Past the largest the zoom gives a little, like a rubber band, and
    // springs back on release; past the smallest, none.
    const double smallest = m_levels.front(), largest = m_levels.back();
    if (shown < smallest)
        shown = smallest;
    else if (shown > largest)
        shown = largest * std::pow(shown / largest, 0.3);
    // Past the point halfway to the next size, and a little more, that one
    // is laid out.
    const auto distance = [&](int level) { return std::abs(std::log(shown / m_levels[level])); };
    if (const int level = nearest(shown); level != m_level && distance(level) < distance(m_level) - kZoomHysteresis) {
        m_level = level;
        emit levelChanged();
    }
    show(shown);
    m_samples.emplace_back(now, std::log(shown));
    while (m_samples.size() > 2 && now - m_samples.front().first > 0.1)
        m_samples.erase(m_samples.begin());
}

void Zoomer::end() {
    if (!m_pinching)
        return;
    m_pinching = false;
    // Where the pinch was heading: its speed at release, carried on briefly.
    double logShown = std::log(m_shown);
    if (m_samples.size() >= 2) {
        const auto &[t0, l0] = m_samples.front();
        const auto &[t1, l1] = m_samples.back();
        if (t1 - t0 > 0.01)
            logShown += (l1 - l0) / (t1 - t0) * kPinchProjection;
    }
    // The size nearest that, but never more than one past the one shown.
    const int lo = std::max(0, m_level - 1), hi = std::min(int(m_levels.size()) - 1, m_level + 1);
    int best = lo;
    for (int i = lo; i <= hi; ++i)
        if (std::abs(std::log(m_levels[i]) - logShown) < std::abs(std::log(m_levels[best]) - logShown))
            best = i;
    settle(best);
}

void Zoomer::settle(int level) {
    if (m_levels.isEmpty())
        return;
    const bool was = zooming();
    m_wheeling = false;
    m_pinching = false;
    level = std::clamp(level, 0, int(m_levels.size()) - 1);
    m_fromShown = m_shown > 0 ? m_shown : size();
    m_toShown = m_levels[level];
    m_t = 0;
    m_settling = true;
    if (level != m_level) {
        m_level = level;
        emit levelChanged();
    }
    show(m_fromShown);
    if (!was)
        emit zoomingChanged();
    startTicking();
}

void Zoomer::zoomBy(int steps) {
    if (m_levels.isEmpty() || steps == 0)
        return;
    if (!m_settling && !m_pinching)
        m_origin = QPointF(width() / 2, height() / 2);
    // From the size it is headed for, as the grid's steps.
    settle(m_level + steps);
}

void Zoomer::wheelZoom(double notches, QPointF at) {
    const double now = seconds();
    const bool rested = now - m_wheelLastAt > kWheelZoomRest;
    m_wheelLastAt = now;
    if (m_wheelSpent && !rested)
        return;
    m_wheelSpent = false;
    if (!m_wheeling) {
        m_wheelFrom = m_level;
        begin(at);
        m_pinching = false;
        m_wheeling = true;
        m_wheelCurrent = m_wheelTarget = std::log(m_shown > 0 ? m_shown : size());
    }
    // No further than the next size either way; there, it lands.
    const int smaller = std::max(m_wheelFrom - 1, 0), larger = std::min(m_wheelFrom + 1, int(m_levels.size()) - 1);
    const double lo = std::log(m_levels[smaller]), hi = std::log(m_levels[larger]);
    m_wheelTarget = std::clamp(m_wheelTarget + notches * std::log(kWheelZoomStep), lo, hi);
    if ((m_wheelTarget <= lo && smaller != m_wheelFrom) || (m_wheelTarget >= hi && larger != m_wheelFrom)) {
        m_wheelSpent = true;
        settle(m_wheelTarget <= lo ? smaller : larger);
        return;
    }
    startTicking();
}

void Zoomer::endWheelZoom() {
    const double from = std::log(m_startShown);
    if (std::abs(m_wheelCurrent - from) < kWheelZoomDead * std::log(kWheelZoomStep)) {
        settle(m_wheelFrom);
        return;
    }
    settle(m_wheelFrom + (m_wheelCurrent > from ? 1 : -1));
}

bool Zoomer::advance(double dt) {
    dt = std::clamp(dt, 0.0, 0.05);
    if (m_wheeling) {
        // The size eases towards where the notches point; still for a
        // moment, it lands on the nearest size as a released pinch does.
        m_wheelCurrent = m_wheelTarget + (m_wheelCurrent - m_wheelTarget) * std::exp(-dt / kWheelZoomEase);
        zoomTo(std::exp(m_wheelCurrent), seconds());
        if (seconds() - m_wheelLastAt > kWheelZoomRest && std::abs(m_wheelCurrent - m_wheelTarget) < 0.005)
            endWheelZoom();
    }
    if (m_settling) {
        m_t = std::min(1.0, m_t + dt / kSettleSeconds);
        // As a ratio, so growing and shrinking feel the same speed.
        show(std::exp(std::log(m_fromShown) + (std::log(m_toShown) - std::log(m_fromShown)) * easeOut(m_t)));
        if (m_t >= 1) {
            m_settling = false;
            show(size());
            emit zoomingChanged();
        }
    }
    return m_settling || m_wheeling;
}

void Zoomer::takeGesture(const QNativeGestureEvent *g) {
    if (m_levels.isEmpty())
        return;
    const QPointF at = mapFromScene(g->scenePosition());
    const double now = g->timestamp() > 0 ? g->timestamp() / 1000.0 : seconds();
    switch (g->gestureType()) {
    case Qt::BeginNativeGesture:
        begin(at);
        break;
    case Qt::ZoomNativeGesture:
        // `value` is the change in scale since the last event; the scale
        // since the pinch began is their sum, as on the grid.
        if (!m_pinching)
            begin(at);
        m_pinchScale = std::clamp(m_pinchScale + g->value(), 0.02, 50.0);
        zoomTo(m_startShown * m_pinchScale, now);
        break;
    case Qt::EndNativeGesture:
        end();
        break;
    default:
        break;
    }
}

void Zoomer::takeWheel(const QWheelEvent *event) {
    if (m_levels.isEmpty())
        return;
    const QPointF at = mapFromScene(event->scenePosition());
    if (event->modifiers() & Qt::ControlModifier) {
        // A notch a size, from the one it is headed for, about the pointer.
        m_notches += event->angleDelta().y() / 120.0;
        if (const int notches = int(m_notches); notches != 0) {
            m_notches -= notches;
            if (!m_settling && !m_pinching)
                m_origin = at;
            settle(m_level + notches);
        }
        return;
    }
    if (event->phase() == Qt::NoScrollPhase && std::abs(event->angleDelta().x()) > std::abs(event->angleDelta().y()))
        wheelZoom(event->angleDelta().x() / 120.0, at);
}

void Zoomer::wheelEvent(QWheelEvent *event) {
    // Its own: Ctrl and the wheel, and the sideways wheel; the rest go on
    // to what is beneath.
    const bool sideways = event->phase() == Qt::NoScrollPhase && std::abs(event->angleDelta().x()) > std::abs(event->angleDelta().y());
    if (!(event->modifiers() & Qt::ControlModifier) && !sideways) {
        event->ignore();
        return;
    }
    event->accept();
    takeWheel(event);
}

bool Zoomer::event(QEvent *event) {
    if (event->type() == QEvent::NativeGesture) {
        takeGesture(static_cast<QNativeGestureEvent *>(event));
        event->accept();
        return true;
    }
    return QQuickItem::event(event);
}

void Zoomer::startTicking() {
    if (m_ticking || !m_window)
        return;
    m_ticking = true;
    m_frameClock.restart();
    m_window->update();
}

void Zoomer::tick() {
    if (!m_ticking)
        return;
    const double dt = m_frameClock.restart() / 1000.0;
    m_ticking = advance(dt);
    if (m_ticking && m_window)
        m_window->update();
}

void Zoomer::itemChange(ItemChange change, const ItemChangeData &value) {
    if (change == ItemSceneChange) {
        if (m_window)
            disconnect(m_frames);
        m_window = value.window;
        if (m_window)
            m_frames = connect(m_window, &QQuickWindow::afterAnimating, this, &Zoomer::tick);
    }
    QQuickItem::itemChange(change, value);
}
