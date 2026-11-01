#pragma once

#include <QQuickItem>

// What a card or a panel over the photos stands on: it takes every press of
// any button, touch, wheel turn, swipe, pinch and hover over it, and does
// nothing with them but say a click, so that what is beneath it (the grid)
// gets none of them. A MouseArea lets the wheel through unless it handles
// it, and every MouseArea lets a pinch through, and the buttons it does not
// accept: a panel standing on one scrolled and zoomed the photos under it.
//
// Under a card's controls, as its first child (or z: -1 under an item's
// own handlers, which then still see a press first); as a scrim beside a
// panel, its click puts the panel away.
//
// Given `steps` (Qt.Horizontal, Qt.Vertical or both), the wheel and the
// touchpad over it step through something, as a slideshow or a carousel
// does: `stepped(1)` for the next (the wheel down, the fingers up or to the
// left), `stepped(-1)` for the one before; a notch a step, a swipe one step
// once it has gone 80 px, and no more until the fingers lift (or rest a
// quarter of a second).
class Shield : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(int steps READ steps WRITE setSteps NOTIFY stepsChanged)
public:
    explicit Shield(QQuickItem *parent = nullptr);
    int steps() const { return m_steps; }
    void setSteps(int steps);

signals:
    // The left button (or a tap) let go over it, where it was pressed on it.
    void clicked();
    void stepped(int step);
    void stepsChanged();

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void mouseUngrabEvent() override;
    void wheelEvent(QWheelEvent *event) override;
    void hoverEnterEvent(QHoverEvent *event) override;
    void hoverMoveEvent(QHoverEvent *event) override;
    bool event(QEvent *event) override;

private:
    bool m_pressed = false;
    int m_steps = 0;
    // A notch's eighths of a degree not yet a step; a swipe's pixels, and
    // whether it stepped, until it rests.
    double m_turned = 0, m_swiped = 0;
    bool m_swipeStepped = false;
    quint64 m_lastSwipe = 0;
};
