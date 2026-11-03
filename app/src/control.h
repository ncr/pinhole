#pragma once

#include <QQuickItem>
#include <QtQml/qqml.h>

// Native TextInput handles Tab before it reaches an ancestor's Keys handler.
// A modal must also fence that native focus chain, including plugin fields.
class FocusFence : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged)
public:
    explicit FocusFence(QObject *parent);
    static FocusFence *qmlAttachedProperties(QObject *object) { return new FocusFence(object); }
    bool enabled() const { return m_enabled; }
    void setEnabled(bool enabled);
signals:
    void enabledChanged();
private:
    bool m_enabled = false;
};
QML_DECLARE_TYPEINFO(FocusFence, QML_HAS_ATTACHED_PROPERTIES)

// An action can be unavailable while its surface must still contain input
// over the photos. Keep Item::enabled for containment; expose usable as the
// disabled state to assistive technology and exclude it from focus/activation.
class ControlSurface : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(bool usable READ usable WRITE setUsable NOTIFY usableChanged)
    Q_PROPERTY(bool keyboardPressed READ keyboardPressed NOTIFY keyboardPressedChanged)
public:
    explicit ControlSurface(QQuickItem *parent = nullptr);
    bool usable() const { return m_usable; }
    void setUsable(bool usable);
    bool keyboardPressed() const { return m_key != 0; }
signals:
    void usableChanged();
    void keyboardPressedChanged();
    void activated();
protected:
    bool event(QEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
private:
    void releaseKey();
    bool m_usable = true;
    int m_key = 0;
};
