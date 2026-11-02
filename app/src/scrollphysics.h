#pragma once

#include <deque>

// Touchpad scrolling that behaves like a Mac: the content follows the fingers,
// faster swipes travel further, a flick keeps coasting and slows down the way
// AppKit's momentum does, flicking again while it coasts speeds it up, and
// the ends stretch like rubber and spring back.
//
// Linux gives none of this: libinput reports finger movement and when the
// fingers lift, nothing more. Hyprland already scales the movement by the
// user's scroll_factor; this adds the rest.
//
// Positions are content pixels, growing toward the end of the library. Times
// are milliseconds. Plain C++, so recorded gestures can be replayed through
// it without a window.
class ScrollPhysics {
public:
    struct Params {
        // Movement at slow speed, before acceleration. Measured on an M2 Pro
        // MacBook Pro with Omarchy's scroll_factor of 0.4: a slow swipe down
        // the whole trackpad (~100 mm) reports ~400 px, while the screen
        // shows ~5 px per mm. 1.3 has a slow swipe move the content about
        // 1.3x as far as the fingers (the acceleration adds a little even
        // when slow): a little ahead, as a Mac does. PINHOLE_SCROLL_GAIN
        // overrides it.
        double trackingGain = 1.3;
        // Acceleration: the gain grows from 1 for slow movement towards
        // 1 + gainExtra for fast swipes. `accelFloor` (px/s) moves at 1:1;
        // `accelScale` sets how quickly the gain rises above it.
        double gainExtra = 3.0;
        double accelFloor = 100;
        double accelScale = 2500;
        // Momentum loses this factor of its speed every millisecond: AppKit
        // and UIKit's normal deceleration.
        double decayPerMs = 0.998;
        double stopBelow = 10;        // px/s
        // Releases slower than this just stop, as on a Mac: putting fingers
        // down to stop a coast moves them a few pixels, and that must not
        // set it drifting again.
        double throwAbove = 300;      // px/s
        double releaseWindowMs = 50;  // movement this recent sets the flick speed
        double holdBeforeLiftMs = 70; // fingers still this long before lifting: no flick
        double boost = 0.8;           // share of the coasting speed a new flick adds
        double maxVelocity = 40000;
        // Rubber band: how far a pull stretches (UIScrollView's 0.55), and
        // the spring that brings it back.
        double rubber = 0.55;
        double springSeconds = 0.42;
        // Mouse wheel: the stiffness (1/s) of the spring that follows it,
        // from its stiffest for a single click or a fast spin, down to its
        // softest for a wheel turned slowly, so a step that comes every
        // 80 ms still blends into the next rather than starting and stopping.
        double wheelStiffest = 30;
        double wheelSoftest = 12;
        // A wheel kept turning is a spin: steps this close (ms), the same
        // way. Turned at a controlled pace each step moves about its full
        // distance; spun faster, the speed levels off at `wheelCruise`
        // (px/s) rather than growing with every step, so a wheel spun
        // freely starts at a speed the eye can follow. A spin of more than
        // `wheelSpinAbove` (px/s of steps) then gathers speed as
        // Shift+Up/Down does: after `wheelRampDelay` seconds, up to
        // `wheelTopSpeed` over `wheelRampSeconds`.
        double wheelSpinGap = 120;
        double wheelCruise = 5000;
        double wheelSpinAbove = 8000;
        double wheelTopSpeed = 16000;
        double wheelRampDelay = 0.4;
        double wheelRampSeconds = 2.5;
    };

    ScrollPhysics() = default;
    explicit ScrollPhysics(const Params &params) : m_p(params) {}

    Params &params() { return m_p; }
    // `extent` is the size of the view, which sets how stiff the rubber band
    // feels; `max` below `min` means everything fits.
    void setBounds(double min, double max, double extent);

    // Fingers touched (a hold, or a click): a coast stops there. If the
    // touch turns into a flick, that flick still builds on the coast.
    void touch(double tMs);
    // Fingers down and moving (`delta` as the touchpad reported it, already
    // in the content's direction), and lifted.
    void begin(double tMs);
    void move(double tMs, double delta);
    void end(double tMs);
    bool tracking() const { return m_tracking; }

    // A mouse wheel notch or keyboard step: eased, never past the ends,
    // with this time constant (a notch's 80 ms by default; longer glides).
    void nudge(double delta, double seconds = 0.08);
    // A mouse wheel turned by `delta` (what one step moves when turned
    // slowly): it glides there with a speed that never jumps, so steps that
    // come every few milliseconds (an MX Master spinning freely) read as one
    // movement rather than a push each, and a spin speeds up the longer it
    // lasts (Params).
    void wheel(double tMs, double delta);
    // Set the position outright; all motion stops.
    void jumpTo(double position);
    // Start coasting at `velocity` px/s, as if just thrown.
    void fling(double velocity);
    void stop();

    // Advance the animation by `dt` seconds. True while still moving.
    bool step(double dt);
    bool animating() const;

    double position() const { return m_position; }
    double velocity() const { return m_velocity; }
    // Where an easing (nudge) is headed; where it is, otherwise.
    double target() const { return m_mode == Mode::Easing ? m_position + m_easeRemaining : m_position; }

private:
    enum class Mode { Idle, Tracking, Coasting, Spring, Easing, Wheel };
    double gain(double speed) const;
    double overscroll() const;
    double rubberBand(double over) const;
    double boundFor(double position) const;
    // Past an end, a spring brings it back, from a coast (`coasting`) or a
    // drag let go there: never further out than a quarter of the view.
    void bounce(bool coasting);

    Params m_p;
    double m_min = 0, m_max = 0, m_extent = 1000;
    Mode m_mode = Mode::Idle;
    bool m_tracking = false;
    double m_position = 0;
    double m_velocity = 0;      // px/s
    double m_unbounded = 0;     // where the fingers would have put it without the rubber band
    double m_easeRemaining = 0;
    double m_easeSeconds = 0.08;
    double m_wheelTarget = 0;
    double m_wheelStiffness = 30;
    double m_wheelInterval = 0;  // ms between recent wheel steps; 0: none recent
    double m_lastWheelAt = -1e9;
    double m_wheelRate = 0;      // px/s of steps in this spin, smoothed
    double m_wheelSpin = 0;      // seconds this spin has been fast
    double m_wheelLastDelta = 0;
    double m_velocityAtBegin = 0;
    double m_caughtVelocity = 0;
    double m_caughtAt = -1e9;
    double m_lastMoveAt = 0;
    struct Sample { double t; double distance; };
    std::deque<Sample> m_samples;
};
