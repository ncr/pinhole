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
class Shield : public QQuickItem {
    Q_OBJECT
public:
    explicit Shield(QQuickItem *parent = nullptr);

signals:
    // The left button (or a tap) let go over it, where it was pressed on it.
    void clicked();

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
};
