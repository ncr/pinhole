#pragma once

#include "scrollphysics.h"

#include <deque>

// How the viewer moves, as in Photos: the photos lie side by side and the
// row follows the fingers, a swipe that goes far or fast enough lands on the
// next one with a spring and any other springs back, and the ends stretch.
// A pinch zooms about the point between the fingers (moving the fingers
// moves the photo too), two fingers pan a zoomed photo the way the grid
// scrolls (following the fingers, coasting after a flick, stretching at the
// photo's edges, stopped by a resting finger), a pinch below the fitted
// size springs back, and one well below it means close.
//
// Positions are window pixels, times milliseconds. Plain C++, so gestures
// can be replayed through it in tests without a window.
class ViewerMotion {
public:
    // The gap between photos side by side.
    static constexpr double kGap = 24;

    void setViewport(double width, double height);
    // The shown photo fitted to the window (zoom 1), and whether it may be
    // zoomed (a video may not).
    void setPhoto(double fittedWidth, double fittedHeight, bool zoomable);
    // Whether there is a photo before and after the shown one.
    void setNeighbours(bool before, bool after);

    // A two-finger touchpad gesture: begin, moves (finger movement), end.
    void begin(double t);
    void move(double t, double dx, double dy);
    // When the gesture paged far or fast enough: -1 or +1, the photo to
    // show next; the caller changes photo and calls stepped(). Else 0.
    int end(double t);

    // The photo changed by `delta` (a swipe, a key, a click): the new one
    // slides in from that side. Zoom goes back to fitted.
    void stepped(int delta);
    // Opening, closing: everything at rest, fitted.
    void reset();

    // A pinch: its scale since it began (1 + the sum of Qt's values) about
    // the point between the fingers.
    void pinchBegin(double x, double y);
    void pinch(double scale, double x, double y);
    // True when pinched well below fitted: close the viewer.
    bool pinchEnd();

    // Zoom by `factor` about a point (Ctrl+wheel, keys), and back to fitted.
    void zoomBy(double factor, double x, double y);
    void resetZoom();
    // Move a zoomed photo (a mouse drag).
    void pan(double dx, double dy);
    // A finger came to rest on the touchpad: a coasting pan stops.
    void touch(double t);

    void advance(double dtSeconds);
    bool animating() const;
    bool zoomed() const { return m_zoom > 1.01 || m_zoomTarget > 1.01; }

    // Where everything is: the row's offset (negative: moved left, the next
    // photo coming in), the shown photo's zoom, and its centre's offset
    // from the window's centre.
    double offset() const { return m_offset; }
    double zoom() const { return m_zoom; }
    double panX() const { return m_panX; }
    double panY() const { return m_panY; }
    double pageWidth() const { return m_width + kGap; }

    // The most a photo zooms, relative to fitted.
    static constexpr double kMaxZoom = 8;

private:
    enum class Gesture { None, Undecided, Page, Pan, Ignore };
    // How far a row stretched past an end shows: a third, softening.
    double stretched(double raw) const;
    void clampPanTargets();
    // The pan's physics take over from wherever the pan is now, within the
    // edges the current zoom allows.
    void panPhysicsFrom(bool keepMotion);
    void stopPanPhysics();
    bool panCoasting() const { return m_panXPhysics.animating() || m_panYPhysics.animating(); }
    double maxPanX(double zoom) const;
    double maxPanY(double zoom) const;

    double m_width = 1, m_height = 1;
    double m_fitW = 1, m_fitH = 1;
    bool m_zoomable = true;
    bool m_before = false, m_after = false;

    Gesture m_gesture = Gesture::None;
    double m_decideX = 0, m_decideY = 0; // movement so far, to tell sideways from not
    double m_raw = 0;     // the row's offset before stretching at the ends
    double m_rawAtBegin = 0; // where this gesture caught the row
    double m_offset = 0;
    double m_velocity = 0; // px/s, the row's, carried into the spring
    struct Sample {
        double t, x;
    };
    std::deque<Sample> m_samples;
    double m_lastMove = 0;

    double m_zoom = 1, m_zoomTarget = 1;
    double m_panX = 0, m_panY = 0, m_panTargetX = 0, m_panTargetY = 0;
    bool m_pinching = false;
    double m_pinchStartZoom = 1;
    double m_pinchX = 0, m_pinchY = 0;
    // While a gesture or pinch holds the zoom and pan, they do not settle.
    bool m_holding = false;
    // Two-finger panning of a zoomed photo, across and down: the grid's
    // scrolling physics.
    ScrollPhysics m_panXPhysics, m_panYPhysics;
};
