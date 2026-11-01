#pragma once

#include <QElapsedTimer>
#include <QPointF>
#include <QPointer>
#include <QQuickItem>

#include <utility>
#include <vector>

class QNativeGestureEvent;
class QWheelEvent;

// A pinch, Ctrl and the wheel, and the sideways wheel, zooming what it holds
// through a few sizes, as the grid zooms through its column counts
// (PhotoGrid: beginZoom, zoomTo, endContinuousZoom, wheelZoom, settleZoom):
// the content grows and shrinks under the fingers, about them, giving a
// little past the largest size and none past the smallest; past the point
// halfway to the next size (and a little more, so it does not flip there)
// that size is the one laid out; let go, it lands on the size nearest where
// the pinch was heading (its speed carried on for 0.12 s, one size past
// those shown at most), eased in 0.3 s. Ctrl and a notch go a size at once;
// the sideways wheel glides, a turn one size at most.
//
// The plugin's PinchZoom (PinchZoom.qml) is one with its content inside,
// drawn at `factor` about `origin` while it zooms: laid out at `size`, a
// size of `levels`, it is drawn as large as the fingers make it.
class Zoomer : public QQuickItem {
    Q_OBJECT
    // The sizes it goes through, smallest first: a cell's, a tile's, a
    // font's; and which one is laid out now.
    Q_PROPERTY(QList<qreal> levels READ levels WRITE setLevels NOTIFY levelsChanged)
    Q_PROPERTY(int level READ level WRITE setLevel NOTIFY levelChanged)
    Q_PROPERTY(qreal size READ size NOTIFY levelChanged)
    // How much larger than `size` it is drawn now (1 at rest), and about
    // which point of it.
    Q_PROPERTY(qreal factor READ factor NOTIFY factorChanged)
    Q_PROPERTY(QPointF origin READ origin NOTIFY factorChanged)
    Q_PROPERTY(bool zooming READ zooming NOTIFY zoomingChanged)
public:
    explicit Zoomer(QQuickItem *parent = nullptr);
    ~Zoomer() override;

    QList<qreal> levels() const { return m_levels; }
    void setLevels(const QList<qreal> &levels);
    int level() const { return m_level; }
    // At once, as it is: no animation.
    void setLevel(int level);
    qreal size() const;
    qreal factor() const { return m_factor; }
    QPointF origin() const { return m_origin; }
    bool zooming() const { return m_pinching || m_settling || m_wheeling; }

    // `steps` sizes larger (or smaller, below 0), eased as a step of the
    // grid's: the keys' + and -.
    Q_INVOKABLE void zoomBy(int steps);

    // What a Scroller within it passes on: a pinch, Ctrl and a notch, the
    // sideways wheel.
    void takeGesture(const QNativeGestureEvent *event);
    void takeWheel(const QWheelEvent *event);

    // One frame of its easing, `dt` seconds (the window's frames, or the
    // physics self-test's). True while still moving.
    bool advance(double dt);
    // The size drawn now: `size` times `factor`.
    double shown() const { return m_shown; }

signals:
    void levelsChanged();
    void levelChanged();
    void factorChanged();
    void zoomingChanged();

protected:
    void wheelEvent(QWheelEvent *event) override;
    bool event(QEvent *event) override;
    void itemChange(ItemChange change, const ItemChangeData &value) override;

private:
    void begin(QPointF at);
    void zoomTo(double shown, double seconds);
    void end();
    void settle(int level);
    void wheelZoom(double notches, QPointF at);
    void endWheelZoom();
    int nearest(double shown) const;
    void show(double shown);
    void tick();
    void startTicking();
    double seconds() const;

    QList<qreal> m_levels;
    int m_level = 0;
    double m_shown = 0;
    double m_factor = 1;
    QPointF m_origin;
    bool m_pinching = false;
    double m_startShown = 0;
    double m_pinchScale = 1;
    std::vector<std::pair<double, double>> m_samples; // (seconds, log size)
    bool m_settling = false;
    double m_fromShown = 0, m_toShown = 0, m_t = 0;
    // The sideways wheel: where it is and is going (log size), from which
    // level, when last turned, and a turn spent at the next level.
    bool m_wheeling = false;
    double m_wheelCurrent = 0, m_wheelTarget = 0, m_wheelLastAt = -1e9;
    int m_wheelFrom = 0;
    bool m_wheelSpent = false;
    double m_notches = 0;
    QPointer<QQuickWindow> m_window;
    QMetaObject::Connection m_frames;
    bool m_ticking = false;
    QElapsedTimer m_frameClock, m_clock;
};
