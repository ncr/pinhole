#include "viewermotion.h"

#include <algorithm>
#include <cmath>

namespace {

// Finger movement to window movement. A trackpad is about 400 px wide in
// these units against a window several times that, so paging takes more
// than the grid's 1.3: a swipe across most of the trackpad carries a photo
// across most of the window, as on a Mac. Panning a zoomed photo follows
// the fingers like scrolling.
constexpr double kPageGain = 2.0;
constexpr double kPanGain = 1.6; // on top of the scroll physics' own gain
// Movement before a gesture is known to be sideways (paging) or not.
constexpr double kDecideAfter = 8;
// A release lands on the next photo past this share of the way, or when
// the fingers were still moving faster than this toward it (px/s).
constexpr double kCommitShare = 0.3;
constexpr double kCommitSpeed = 400;
// The row's spring: critically damped, settling in about a third of a
// second. Zoom and pan settle by easing out at this rate per second.
constexpr double kSpring = 14;
constexpr double kEase = 16;
// The carousel's own spring (jumped()), and how much stiffer it gets for
// each photo it is behind.
constexpr double kStripSpring = 20;
constexpr double kStripCatchUp = 10;
// A held key: how long before the photos move on by themselves (s), how
// many a second they start at, the most (without and with Shift), and how
// quickly they get there (the share left shrinks by e^-rate a second):
// speeding up gently, slowing down at once when Shift is let go.
constexpr double kHoldDelay = 0.35;
constexpr double kHoldStart = 3;
constexpr double kHoldTop = 20;
constexpr double kHoldTopFast = 60;
constexpr double kHoldRate = 1.2;
constexpr double kHoldBrake = 8;
// Pinched below this share of fitted on release: close.
constexpr double kCloseBelow = 0.7;
// Swiped up or down: the photo follows like this, and closing takes a drag
// past this share of the window's height, or a flick this fast (px/s).
constexpr double kDismissGain = 1.6;
constexpr double kDismissShare = 0.12;
constexpr double kDismissSpeed = 700;

// A zoomed photo's coast is short: UIKit's "fast" deceleration, which
// image viewers use, about a fifth of the scrolling coast's distance and
// over in half a second, not the grid's long AppKit glide.
constexpr double kPanDecayPerMs = 0.99;

} // namespace

ViewerMotion::ViewerMotion() {
    m_panXPhysics.params().decayPerMs = kPanDecayPerMs;
    m_panYPhysics.params().decayPerMs = kPanDecayPerMs;
}

void ViewerMotion::setViewport(double width, double height) {
    m_width = std::max(1.0, width);
    m_height = std::max(1.0, height);
    clampPanTargets();
}

void ViewerMotion::setPhoto(double fittedWidth, double fittedHeight, bool zoomable) {
    m_fitW = std::max(1.0, fittedWidth);
    m_fitH = std::max(1.0, fittedHeight);
    m_zoomable = zoomable;
}

void ViewerMotion::setNeighbours(bool before, bool after) {
    m_before = before;
    m_after = after;
}

double ViewerMotion::stretched(double raw) const {
    const bool pastEnd = (raw > 0 && !m_before) || (raw < 0 && !m_after);
    if (!pastEnd)
        return raw;
    // As iOS stretches: ever less for each further pixel, never a page.
    const double d = pageWidth();
    const double x = std::abs(raw);
    return std::copysign((1 - 1 / (x * 0.55 / d + 1)) * d, raw);
}

double ViewerMotion::maxPanX(double zoom) const { return std::max(0.0, (m_fitW * zoom - m_width) / 2); }
double ViewerMotion::maxPanY(double zoom) const { return std::max(0.0, (m_fitH * zoom - m_height) / 2); }

void ViewerMotion::clampPanTargets() {
    m_panTargetX = std::clamp(m_panTargetX, -maxPanX(m_zoomTarget), maxPanX(m_zoomTarget));
    m_panTargetY = std::clamp(m_panTargetY, -maxPanY(m_zoomTarget), maxPanY(m_zoomTarget));
}

void ViewerMotion::begin(double t) {
    if (m_pinching)
        return;
    m_samples.clear();
    m_lastMove = t;
    m_decideX = m_decideY = 0;
    if (zoomed()) {
        // A zoomed photo pans, catching a coast that is still going.
        m_gesture = Gesture::Pan;
        m_holding = true;
        panPhysicsFrom(true);
        m_panXPhysics.begin(t);
        m_panYPhysics.begin(t);
        return;
    }
    m_gesture = Gesture::Undecided;
    // A row still springing is caught where it is (the carousel too).
    m_raw = m_offset;
    m_stripExtra = m_stripExtraStart = 0;
    m_rawAtBegin = m_raw;
    m_velocity = 0;
}

void ViewerMotion::move(double t, double dx, double dy) {
    switch (m_gesture) {
    case Gesture::Undecided:
        m_decideX += dx;
        m_decideY += dy;
        if (std::hypot(m_decideX, m_decideY) < kDecideAfter)
            return;
        if (std::abs(m_decideX) > std::abs(m_decideY)) {
            m_gesture = Gesture::Page;
            dx = m_decideX;
        } else {
            // Up or down: dragging the photo away, to close.
            m_gesture = Gesture::Dismiss;
            m_dismissSamples.clear();
            m_dismissVelocity = 0;
            dy = m_decideY;
            m_dismiss += dy * kDismissGain;
            m_dismissSamples.push_back({t, m_dismiss});
            return;
        }
        [[fallthrough]];
    case Gesture::Page:
        m_raw += dx * kPageGain;
        m_offset = stretched(m_raw);
        m_samples.push_back({t, m_raw});
        while (m_samples.size() > 2 && t - m_samples.front().t > 100)
            m_samples.pop_front();
        m_lastMove = t;
        return;
    case Gesture::Pan:
        // Fingers moving right move the photo right.
        m_panXPhysics.move(t, dx * kPanGain);
        m_panYPhysics.move(t, dy * kPanGain);
        m_panX = m_panTargetX = m_panXPhysics.position();
        m_panY = m_panTargetY = m_panYPhysics.position();
        return;
    case Gesture::Dismiss:
        m_dismiss += dy * kDismissGain;
        m_dismissSamples.push_back({t, m_dismiss});
        while (m_dismissSamples.size() > 2 && t - m_dismissSamples.front().t > 100)
            m_dismissSamples.pop_front();
        m_lastMove = t;
        return;
    case Gesture::None:
        return;
    }
}

int ViewerMotion::end(double t) {
    const Gesture was = m_gesture;
    m_gesture = Gesture::None;
    if (was == Gesture::Dismiss) {
        double v = 0;
        if (m_dismissSamples.size() >= 2 && t - m_lastMove < 60) {
            const Sample &a = m_dismissSamples.front(), &b = m_dismissSamples.back();
            if (b.t > a.t)
                v = (b.x - a.x) / (b.t - a.t) * 1000;
        }
        // Far enough, or flicked on the way out (not back): close. Else it
        // springs back (advance()).
        const bool outward = (m_dismiss > 0 && v > 0) || (m_dismiss < 0 && v < 0);
        if (std::abs(m_dismiss) > kDismissShare * m_height || (outward && std::abs(v) > kDismissSpeed))
            m_close = true;
        else
            m_dismissVelocity = v;
        return 0;
    }
    if (was == Gesture::Pan) {
        // Thrown, it coasts; past an edge it springs back (advance()).
        m_panXPhysics.end(t);
        m_panYPhysics.end(t);
        m_holding = m_pinching;
        return 0;
    }
    if (was != Gesture::Page)
        return 0;
    // How fast the fingers were going as they lifted; nothing if they had
    // stopped first.
    double v = 0;
    if (m_samples.size() >= 2 && t - m_lastMove < 60) {
        const Sample &a = m_samples.front(), &b = m_samples.back();
        if (b.t > a.t)
            v = (b.x - a.x) / (b.t - a.t) * 1000;
    }
    m_velocity = v;
    // A flick pages its way. A slow release pages when the row is past the
    // threshold in the direction the fingers moved: a second swipe caught
    // the row still sliding from the first, so where the row lies alone
    // says nothing about which way this one went.
    const double page = pageWidth();
    const double moved = m_raw - m_rawAtBegin;
    if (m_after && (v < -kCommitSpeed || (moved < 0 && m_offset < -kCommitShare * page)))
        return 1;
    if (m_before && (v > kCommitSpeed || (moved > 0 && m_offset > kCommitShare * page)))
        return -1;
    return 0;
}

void ViewerMotion::panPhysicsFrom(bool keepMotion) {
    m_panXPhysics.setBounds(-maxPanX(m_zoom), maxPanX(m_zoom), m_width);
    m_panYPhysics.setBounds(-maxPanY(m_zoom), maxPanY(m_zoom), m_height);
    if (!(keepMotion && panCoasting())) {
        m_panXPhysics.jumpTo(m_panX);
        m_panYPhysics.jumpTo(m_panY);
    }
}

void ViewerMotion::stopPanPhysics() {
    // Everything stops, a spring back from an edge included (ScrollPhysics
    // stop() keeps that one going): what takes over (a zoom, a pinch, the
    // easing to the edges) brings the photo within them itself.
    m_panXPhysics.jumpTo(m_panX);
    m_panYPhysics.jumpTo(m_panY);
}

void ViewerMotion::touch(double t) {
    m_panXPhysics.touch(t);
    m_panYPhysics.touch(t);
    m_panX = m_panTargetX = m_panXPhysics.position();
    m_panY = m_panTargetY = m_panYPhysics.position();
}

void ViewerMotion::stopCoasting() {
    if (!panCoasting())
        return;
    stopPanPhysics();
    // Stopped past an edge, it still goes back within it.
    m_panTargetX = m_panX;
    m_panTargetY = m_panY;
    clampPanTargets();
}

void ViewerMotion::stepped(int delta, int rowDelta) {
    const double page = pageWidth();
    const double whole = stripOffset() + delta * page;
    m_offset = std::clamp(m_offset + rowDelta * page, -1.5 * page, 1.5 * page);
    m_raw = m_offset;
    m_offsetStart = m_offset;
    m_stripExtraStart = m_stripExtra = whole - m_offset;
    m_strip = m_stripVelocity = 0;
    m_zoom = m_zoomTarget = 1;
    m_panX = m_panY = m_panTargetX = m_panTargetY = 0;
    m_holding = false;
    stopPanPhysics();
}

void ViewerMotion::jumped(int delta) {
    // The carousel goes on from where it shows now, the row's slide
    // included, keeping its speed.
    const double strip = stripOffset() + delta * pageWidth();
    const double velocity = m_stripVelocity;
    reset();
    m_strip = strip;
    m_stripVelocity = velocity;
}

void ViewerMotion::hold(int direction, bool fast) {
    m_holdDir = direction > 0 ? 1 : -1;
    m_holdFast = fast;
    m_holdTime = 0;
    m_holdSpeed = 0;
    m_heldSteps = 0;
}

void ViewerMotion::release() {
    // The carousel's spring takes over from where it is, at its speed.
    m_holdDir = 0;
    m_holdSpeed = 0;
}

void ViewerMotion::reset() {
    stopPanPhysics();
    m_dismiss = m_dismissVelocity = 0;
    m_close = false;
    m_stripExtra = m_stripExtraStart = m_offsetStart = 0;
    m_strip = m_stripVelocity = 0;
    m_gesture = Gesture::None;
    m_offset = m_raw = m_velocity = 0;
    m_zoom = m_zoomTarget = 1;
    m_panX = m_panY = m_panTargetX = m_panTargetY = 0;
    m_pinching = m_holding = false;
    m_samples.clear();
}

void ViewerMotion::pinchBegin(double x, double y) {
    stopPanPhysics();
    m_pinching = true;
    m_holding = true;
    m_gesture = Gesture::None;
    // A row mid-spring lands where it was going; zooming starts from where
    // the zoom is now.
    m_pinchStartZoom = m_zoom;
    m_pinchX = x;
    m_pinchY = y;
}

void ViewerMotion::pinch(double scale, double x, double y) {
    if (!m_pinching)
        pinchBegin(x, y);
    const double top = m_zoomable ? kMaxZoom * 1.3 : 1.15;
    const double zoom = std::clamp(m_pinchStartZoom * scale, 0.3, top);
    // Fingers moving together move the photo; then the zoom keeps the point
    // between them where it is.
    m_panX += x - m_pinchX;
    m_panY += y - m_pinchY;
    m_pinchX = x;
    m_pinchY = y;
    const double cx = m_width / 2 + m_panX, cy = m_height / 2 + m_panY;
    const double r = zoom / m_zoom;
    m_panX = x + (cx - x) * r - m_width / 2;
    m_panY = y + (cy - y) * r - m_height / 2;
    m_zoom = zoom;
    m_zoomTarget = zoom;
    m_panTargetX = m_panX;
    m_panTargetY = m_panY;
}

bool ViewerMotion::pinchEnd() {
    if (!m_pinching)
        return false;
    m_pinching = false;
    m_holding = m_gesture == Gesture::Pan;
    if (m_zoom < kCloseBelow)
        return true;
    const double top = m_zoomable ? kMaxZoom : 1;
    const double target = std::clamp(m_zoom, 1.0, top);
    if (target != m_zoom) {
        // Back within bounds about the point the fingers left.
        const double cx = m_width / 2 + m_panX, cy = m_height / 2 + m_panY;
        const double r = target / m_zoom;
        m_panTargetX = m_pinchX + (cx - m_pinchX) * r - m_width / 2;
        m_panTargetY = m_pinchY + (cy - m_pinchY) * r - m_height / 2;
    }
    m_zoomTarget = target;
    if (target <= 1)
        m_panTargetX = m_panTargetY = 0;
    clampPanTargets();
    return false;
}

void ViewerMotion::zoomBy(double factor, double x, double y) {
    stopPanPhysics();
    const double top = m_zoomable ? kMaxZoom : 1;
    const double target = std::clamp(m_zoomTarget * factor, 1.0, top);
    const double cx = m_width / 2 + m_panTargetX, cy = m_height / 2 + m_panTargetY;
    const double r = target / m_zoomTarget;
    m_panTargetX = x + (cx - x) * r - m_width / 2;
    m_panTargetY = y + (cy - y) * r - m_height / 2;
    m_zoomTarget = target;
    if (target <= 1)
        m_panTargetX = m_panTargetY = 0;
    clampPanTargets();
}

void ViewerMotion::resetZoom() {
    stopPanPhysics();
    m_zoomTarget = 1;
    m_panTargetX = m_panTargetY = 0;
}

void ViewerMotion::pan(double dx, double dy) {
    if (!zoomed())
        return;
    stopPanPhysics();
    m_panX = std::clamp(m_panX + dx, -maxPanX(m_zoom), maxPanX(m_zoom));
    m_panY = std::clamp(m_panY + dy, -maxPanY(m_zoom), maxPanY(m_zoom));
    m_panTargetX = m_panX;
    m_panTargetY = m_panY;
}

bool ViewerMotion::animating() const {
    const bool row = m_gesture != Gesture::Page && (m_offset != 0 || m_velocity != 0);
    const bool strip = m_strip != 0 || m_stripVelocity != 0 || m_holdDir != 0;
    const bool dismiss = m_gesture != Gesture::Dismiss && !m_close && m_dismiss != 0;
    const bool zoom = !m_holding && (m_zoom != m_zoomTarget || m_panX != m_panTargetX || m_panY != m_panTargetY);
    return row || strip || zoom || dismiss || panCoasting();
}

void ViewerMotion::advance(double dt) {
    dt = std::clamp(dt, 0.0, 0.05);
    if (m_gesture != Gesture::Page && (m_offset != 0 || m_velocity != 0)) {
        const double before = m_offset;
        const double a = -kSpring * kSpring * m_offset - 2 * kSpring * m_velocity;
        m_velocity += a * dt;
        m_offset += m_velocity * dt;
        // Never through the photo it is settling on and out the other side.
        if ((before > 0 && m_offset < 0) || (before < 0 && m_offset > 0) ||
            (std::abs(m_offset) < 0.5 && std::abs(m_velocity) < 20)) {
            m_offset = 0;
            m_velocity = 0;
        }
        m_raw = m_offset;
        m_stripExtra = m_offsetStart != 0 ? m_stripExtraStart * m_offset / m_offsetStart : 0;
    }
    // The carousel after a photo changed at once: a spring stiffer the
    // more photos behind it is, so a held key (40 photos a second) keeps it
    // a photo or two behind rather than five.
    m_holdTime += m_holdDir != 0 ? dt : 0;
    if (m_holdDir != 0 && m_holdTime >= kHoldDelay) {
        // Held: the carousel moves on at the held speed, from wherever the
        // press left it, a photo behind the shown one at most; reaching it
        // brings the next (one a frame at most).
        const double page = pageWidth();
        const double top = m_holdFast ? kHoldTopFast : kHoldTop;
        m_holdSpeed = std::max(m_holdSpeed, kHoldStart);
        const double rate = m_holdSpeed > top ? kHoldBrake : kHoldRate;
        m_holdSpeed += (top - m_holdSpeed) * (1 - std::exp(-rate * dt));
        m_stripVelocity = -m_holdDir * m_holdSpeed * page;
        m_strip += m_stripVelocity * dt;
        if (m_holdDir * m_strip <= 0) {
            if ((m_holdDir > 0 ? m_after : m_before) && m_heldSteps == 0) {
                m_heldSteps = m_holdDir;
                m_strip = m_holdDir * std::max(0.0, page + m_holdDir * m_strip);
            } else {
                // The end of the library, or a frame too slow for the speed.
                m_strip = 0;
                if (!(m_holdDir > 0 ? m_after : m_before))
                    m_stripVelocity = 0;
            }
        }
    } else if (m_strip != 0 || m_stripVelocity != 0) {
        const double before = m_strip;
        const double w = kStripSpring + kStripCatchUp * std::abs(m_strip) / pageWidth();
        m_stripVelocity += (-w * w * m_strip - 2 * w * m_stripVelocity) * dt;
        m_strip += m_stripVelocity * dt;
        if ((before > 0 && m_strip < 0) || (before < 0 && m_strip > 0) ||
            (std::abs(m_strip) < 0.5 && std::abs(m_stripVelocity) < 20)) {
            m_strip = 0;
            m_stripVelocity = 0;
        }
    }
    // Let go short of closing: back into place, as the row springs.
    if (m_gesture != Gesture::Dismiss && !m_close && m_dismiss != 0) {
        const double before = m_dismiss;
        const double a = -kSpring * kSpring * m_dismiss - 2 * kSpring * m_dismissVelocity;
        m_dismissVelocity += a * dt;
        m_dismiss += m_dismissVelocity * dt;
        if ((before > 0 && m_dismiss < 0) || (before < 0 && m_dismiss > 0) ||
            (std::abs(m_dismiss) < 0.5 && std::abs(m_dismissVelocity) < 20)) {
            m_dismiss = 0;
            m_dismissVelocity = 0;
        }
    }
    if (panCoasting() && !m_pinching) {
        // Coasting, or springing back from an edge, within the edges of the
        // zoom as it is now.
        m_panXPhysics.setBounds(-maxPanX(m_zoom), maxPanX(m_zoom), m_width);
        m_panYPhysics.setBounds(-maxPanY(m_zoom), maxPanY(m_zoom), m_height);
        m_panXPhysics.step(dt);
        m_panYPhysics.step(dt);
        m_panX = m_panTargetX = m_panXPhysics.position();
        m_panY = m_panTargetY = m_panYPhysics.position();
    }
    if (!m_holding) {
        const double f = 1 - std::exp(-kEase * dt);
        auto ease = [f](double &value, double target, double snap) {
            value += (target - value) * f;
            if (std::abs(target - value) < snap)
                value = target;
        };
        ease(m_zoom, m_zoomTarget, 0.001);
        ease(m_panX, m_panTargetX, 0.25);
        ease(m_panY, m_panTargetY, 0.25);
    }
}
