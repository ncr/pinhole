#pragma once

#include "scrollphysics.h"

#include <deque>
#include <utility>

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
    ViewerMotion();

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

    // The photo changed by `delta` (a swipe, a far tap on the carousel):
    // the new one slides in from that side, the row sliding `rowDelta`
    // photos (by default all of them); the carousel travels all `delta`.
    // Zoom goes back to fitted.
    void stepped(int delta, int rowDelta);
    void stepped(int delta) { stepped(delta, delta); }
    // The photo changed by `delta` at once (a key, a click): the row is at
    // rest on it straight away, and only the carousel glides after it, from
    // wherever it was, quicker the further behind it is, so a held key
    // scrolls it evenly a photo or two behind and it lands when let go.
    void jumped(int delta);
    // A key held after its press (which jumped()): a moment later the
    // carousel starts moving on by itself, a photo every half second, and
    // with Shift gathers speed; takeSteps() says when the next photo is
    // due. The system's key repeat plays no part, so it is the same for
    // everyone.
    void hold(int direction, bool fast);
    void setHoldFast(bool fast) { m_holdFast = fast; }
    // Let go: the carousel lands on the photo shown, from the speed it had.
    void release();
    bool holding() const { return m_holdDir != 0; }
    // Photos to move on by since last asked (a held key), -1, 0 or 1.
    int takeSteps() { return std::exchange(m_heldSteps, 0); }
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
    // A press or tap: a coasting pan stops where it is.
    void stopCoasting();

    void advance(double dtSeconds);
    bool animating() const;
    bool zoomed() const { return m_zoom > 1.01 || m_zoomTarget > 1.01; }

    // Where everything is: the row's offset (negative: moved left, the next
    // photo coming in), the shown photo's zoom, and its centre's offset
    // from the window's centre.
    double offset() const { return m_offset; }
    // The same for the carousel, which travels the whole way when a step
    // goes further than the photos slide (they come from 1.5 pages out),
    // and still glides after a photo changed at once (jumped()).
    double stripOffset() const { return m_offset + m_stripExtra + m_strip; }
    double zoom() const { return m_zoom; }
    double panX() const { return m_panX; }
    double panY() const { return m_panY; }
    double pageWidth() const { return m_width + kGap; }
    // A vertical swipe on an unzoomed photo drags it up or down (this far),
    // to close the viewer; takeClose() says once when it should.
    double dismiss() const { return m_dismiss; }
    bool takeClose() { return std::exchange(m_close, false); }

    // The most a photo zooms, relative to fitted.
    static constexpr double kMaxZoom = 8;

private:
    enum class Gesture { None, Undecided, Page, Pan, Dismiss };
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
    // A long step: how much further the carousel has to travel than the
    // photos, shrinking in proportion as they settle.
    double m_stripExtra = 0, m_stripExtraStart = 0, m_offsetStart = 0;
    // How far the carousel is behind a photo changed at once, and how fast
    // it is catching up (px/s), on a spring of its own.
    double m_strip = 0, m_stripVelocity = 0;
    // A held key: its direction (0: none), whether with Shift, how long it
    // has been down (s), how fast the photos pass (per s), and a step due.
    int m_holdDir = 0;
    bool m_holdFast = false;
    double m_holdTime = 0, m_holdSpeed = 0;
    int m_heldSteps = 0;
    struct Sample {
        double t, x;
    };
    std::deque<Sample> m_samples;
    double m_lastMove = 0;
    double m_dismiss = 0, m_dismissVelocity = 0;
    std::deque<Sample> m_dismissSamples;
    bool m_close = false;

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
