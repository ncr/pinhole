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
// Pinched below this share of fitted on release: close.
constexpr double kCloseBelow = 0.7;

} // namespace

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
    // A row still springing is caught where it is.
    m_raw = m_offset;
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
            m_gesture = Gesture::Ignore;
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
    case Gesture::None:
    case Gesture::Ignore:
        return;
    }
}

int ViewerMotion::end(double t) {
    const Gesture was = m_gesture;
    m_gesture = Gesture::None;
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
    m_panXPhysics.stop();
    m_panYPhysics.stop();
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

void ViewerMotion::stepped(int delta) {
    const double page = pageWidth();
    m_offset = std::clamp(m_offset + delta * page, -1.5 * page, 1.5 * page);
    m_raw = m_offset;
    m_zoom = m_zoomTarget = 1;
    m_panX = m_panY = m_panTargetX = m_panTargetY = 0;
    m_holding = false;
    stopPanPhysics();
}

void ViewerMotion::reset() {
    stopPanPhysics();
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
    const bool zoom = !m_holding && (m_zoom != m_zoomTarget || m_panX != m_panTargetX || m_panY != m_panTargetY);
    return row || zoom || panCoasting();
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
