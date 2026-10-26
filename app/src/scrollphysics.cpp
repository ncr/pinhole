#include "scrollphysics.h"

#include <algorithm>
#include <cmath>

namespace {
// Typical time between two touchpad reports (120 Hz). A flick's speed is
// distance over the reports' span plus one interval, so two reports 1 ms
// apart do not read as a 100,000 px/s flick.
constexpr double kReportMs = 8;
} // namespace

void ScrollPhysics::setBounds(double min, double max, double extent) {
    m_min = min;
    m_max = std::max(min, max);
    m_extent = std::max(1.0, extent);
    // The ends moved (a resize, a new column count): settle back inside.
    if (!m_tracking && overscroll() != 0 && m_mode != Mode::Spring)
        m_mode = Mode::Spring;
}

double ScrollPhysics::gain(double speed) const {
    const double over = std::max(0.0, speed - m_p.accelFloor);
    return 1 + m_p.gainExtra * (1 - std::exp(-over / m_p.accelScale));
}

double ScrollPhysics::boundFor(double position) const { return std::clamp(position, m_min, m_max); }

double ScrollPhysics::overscroll() const { return m_position - boundFor(m_position); }

double ScrollPhysics::rubberBand(double over) const {
    // UIScrollView's: the further it is pulled, the less it gives, never
    // past one view's length.
    return (1 - 1 / (over * m_p.rubber / m_extent + 1)) * m_extent;
}

void ScrollPhysics::touch(double tMs) {
    if (m_mode != Mode::Coasting)
        return;
    m_caughtVelocity = m_velocity;
    m_caughtAt = tMs;
    m_velocity = 0;
    m_mode = Mode::Idle;
}

void ScrollPhysics::begin(double tMs) {
    // Catching a flick: whatever was coasting stops under the fingers, but
    // is remembered so a flick in the same direction can build on it. A
    // touch just before (a hold that turned into this scroll) counts too.
    if (m_mode == Mode::Coasting)
        m_velocityAtBegin = m_velocity;
    else
        m_velocityAtBegin = tMs - m_caughtAt < 400 ? m_caughtVelocity : 0;
    m_caughtVelocity = 0;
    m_mode = Mode::Tracking;
    m_tracking = true;
    m_velocity = 0;
    m_easeRemaining = 0;
    m_samples.clear();
    m_lastMoveAt = tMs;
    // Pick up a stretched rubber band where it is: undo the stretch so the
    // next movement continues from the same spot.
    const double over = overscroll();
    if (over == 0) {
        m_unbounded = m_position;
    } else {
        const double y = std::min(std::abs(over), m_extent * 0.999);
        const double x = (1 / (1 - y / m_extent) - 1) * m_extent / m_p.rubber;
        m_unbounded = boundFor(m_position) + (over < 0 ? -x : x);
    }
}

void ScrollPhysics::move(double tMs, double delta) {
    if (!m_tracking)
        begin(tMs);
    const double dt = std::max(1.0, tMs - m_lastMoveAt);
    const double speed = std::abs(delta) / dt * 1000;
    const double moved = delta * m_p.trackingGain * gain(speed);
    m_unbounded += moved;
    if (m_unbounded < m_min)
        m_position = m_min - rubberBand(m_min - m_unbounded);
    else if (m_unbounded > m_max)
        m_position = m_max + rubberBand(m_unbounded - m_max);
    else
        m_position = m_unbounded;
    m_samples.push_back({tMs, moved});
    while (!m_samples.empty() && tMs - m_samples.front().t > m_p.releaseWindowMs * 3)
        m_samples.pop_front();
    m_lastMoveAt = tMs;
}

void ScrollPhysics::end(double tMs) {
    if (!m_tracking)
        return;
    m_tracking = false;

    // Fingers that stopped before lifting put it down; fingers that lift
    // while moving throw it.
    double v = 0;
    if (!m_samples.empty() && tMs - m_lastMoveAt <= m_p.holdBeforeLiftMs) {
        double distance = 0;
        double first = m_lastMoveAt;
        for (const Sample &s : m_samples) {
            if (m_lastMoveAt - s.t <= m_p.releaseWindowMs) {
                distance += s.distance;
                first = std::min(first, s.t);
            }
        }
        v = distance / std::max(2 * kReportMs, m_lastMoveAt - first + kReportMs) * 1000;
    }
    // A flick on top of a coasting flick, same way: it speeds up, so long
    // distances take a few quick flicks rather than many slow ones.
    if (m_velocityAtBegin != 0 && v != 0 && (v > 0) == (m_velocityAtBegin > 0) && std::abs(v) > 300)
        v += m_p.boost * m_velocityAtBegin;
    m_velocity = std::clamp(v, -m_p.maxVelocity, m_p.maxVelocity);
    m_samples.clear();

    if (overscroll() != 0)
        m_mode = Mode::Spring;
    else if (std::abs(m_velocity) >= m_p.throwAbove)
        m_mode = Mode::Coasting;
    else
        m_mode = Mode::Idle, m_velocity = 0;
}

void ScrollPhysics::nudge(double delta, double seconds) {
    m_easeSeconds = seconds;
    if (m_mode != Mode::Easing) {
        m_mode = Mode::Easing;
        m_velocity = 0;
        m_easeRemaining = 0;
    }
    const double target = boundFor(boundFor(m_position) + m_easeRemaining + delta);
    m_easeRemaining = target - m_position;
}

void ScrollPhysics::wheel(double tMs, double delta) {
    const double gap = tMs - m_lastWheelAt;
    m_lastWheelAt = tMs;
    const bool spinning = gap <= m_p.wheelSpinGap && (delta > 0) == (m_wheelLastDelta > 0);
    m_wheelLastDelta = delta;
    double moved = delta;
    if (!spinning) {
        m_wheelRate = 0;
        m_wheelSpin = 0;
    } else {
        // Two reports in one millisecond are one frame's steps, not a
        // wheel turning a thousand times faster.
        const double g = std::max(4.0, gap);
        const double rate = std::abs(delta) / g * 1000;
        m_wheelRate = m_wheelRate == 0 ? rate : m_wheelRate + (rate - m_wheelRate) * (1 - std::exp(-g / 80));
        // Fast, it gathers speed; slowing down, it loses it twice as fast,
        // so spinning the wheel again after it slowed does not shoot off at
        // the speed it had.
        if (m_wheelRate >= m_p.wheelSpinAbove)
            m_wheelSpin += g / 1000;
        else
            m_wheelSpin = std::max(0.0, m_wheelSpin - 2 * g / 1000);
        // The speed levels off at the cruise: about 1 at a controlled pace,
        // cruise / rate when spun.
        const double r = m_wheelRate / m_p.wheelCruise;
        const double levelled = r > 1e-6 ? std::tanh(r) / r : 1;
        const double ramp = std::clamp((m_wheelSpin - m_p.wheelRampDelay) / m_p.wheelRampSeconds, 0.0, 1.0);
        const double boost = 1 + (m_p.wheelTopSpeed / m_p.wheelCruise - 1) * ramp * ramp * (3 - 2 * ramp);
        moved = delta * levelled * boost;
    }

    // How fast the steps come sets how soft the spring is: about the time
    // between two of them, so each one carries on where the last leaves off.
    if (gap > 150)
        m_wheelInterval = 0;
    else
        m_wheelInterval = m_wheelInterval == 0 ? std::max(1.0, gap) : 0.6 * m_wheelInterval + 0.4 * std::max(1.0, gap);
    m_wheelStiffness = m_wheelInterval == 0
                           ? m_p.wheelStiffest
                           : std::clamp(1000 / m_wheelInterval, m_p.wheelSoftest, m_p.wheelStiffest);
    if (m_mode != Mode::Wheel) {
        m_mode = Mode::Wheel;
        m_velocity = 0;
        m_easeRemaining = 0;
        m_wheelTarget = boundFor(m_position);
    }
    m_wheelTarget = boundFor(m_wheelTarget + moved);
}

void ScrollPhysics::jumpTo(double position) {
    m_position = position;
    m_unbounded = position;
    m_velocity = 0;
    m_easeRemaining = 0;
    m_samples.clear();
    m_tracking = false;
    m_mode = Mode::Idle;
}

void ScrollPhysics::fling(double velocity) {
    m_tracking = false;
    m_easeRemaining = 0;
    m_velocity = std::clamp(velocity, -m_p.maxVelocity, m_p.maxVelocity);
    m_mode = m_velocity == 0 ? Mode::Idle : Mode::Coasting;
}

void ScrollPhysics::stop() {
    m_velocity = 0;
    m_easeRemaining = 0;
    m_mode = overscroll() != 0 && !m_tracking ? Mode::Spring : (m_tracking ? Mode::Tracking : Mode::Idle);
}

bool ScrollPhysics::animating() const {
    return m_mode == Mode::Coasting || m_mode == Mode::Spring || m_mode == Mode::Easing || m_mode == Mode::Wheel;
}

bool ScrollPhysics::step(double dt) {
    if (dt <= 0)
        return animating();
    // Small fixed substeps keep the spring stable however uneven frames are.
    const double h = 0.001;
    double left = dt;
    while (left > 1e-9 && animating()) {
        const double s = std::min(h, left);
        left -= s;
        switch (m_mode) {
        case Mode::Coasting:
            m_position += m_velocity * s;
            m_velocity *= std::pow(m_p.decayPerMs, s * 1000);
            if (overscroll() != 0) {
                m_mode = Mode::Spring; // ran into an end: bounce
            } else if (std::abs(m_velocity) < m_p.stopBelow) {
                m_velocity = 0;
                m_mode = Mode::Idle;
            }
            break;
        case Mode::Spring: {
            // Critically damped: it gives a little, then settles without
            // wobbling, in about `springSeconds`.
            const double omega = 5.0 / m_p.springSeconds;
            const double target = boundFor(m_position);
            const double x = m_position - target;
            const double a = -omega * omega * x - 2 * omega * m_velocity;
            m_velocity += a * s;
            m_position += m_velocity * s;
            if (std::abs(m_position - boundFor(m_position)) < 0.5 && std::abs(m_velocity) < 20) {
                m_position = boundFor(m_position);
                m_velocity = 0;
                m_mode = Mode::Idle;
            }
            break;
        }
        case Mode::Easing: {
            // A wheel notch eases in over about 80 ms; a glide, longer.
            const double moved = m_easeRemaining * (1 - std::exp(-s / m_easeSeconds));
            m_position += moved;
            m_easeRemaining -= moved;
            if (std::abs(m_easeRemaining) < 0.5) {
                m_position += m_easeRemaining;
                m_easeRemaining = 0;
                m_mode = Mode::Idle;
            }
            break;
        }
        case Mode::Wheel: {
            // Critically damped towards where the wheel points: a new step
            // moves the target, never the speed, so there is no jolt.
            const double omega = m_wheelStiffness;
            const double a = omega * omega * (m_wheelTarget - m_position) - 2 * omega * m_velocity;
            m_velocity += a * s;
            m_position = boundFor(m_position + m_velocity * s);
            if (std::abs(m_wheelTarget - m_position) < 0.5 && std::abs(m_velocity) < 20) {
                m_position = m_wheelTarget;
                m_velocity = 0;
                m_mode = Mode::Idle;
            }
            break;
        }
        default:
            break;
        }
    }
    m_unbounded = m_position;
    return animating();
}
