#pragma once

#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QVariantList>

// Every action of the window, each a Command (Command.qml), as one list: the
// keys that do it, and how it is done with a mouse and with a touchpad. The
// keys work through it and the help is made from it, so neither can drift
// from what Pinhole does.
//
// The keys belong to one layer at a time, the one on top: the grid, the
// viewer, a menu, a panel, sign-in, a tour, the help, a plugin's screen
// (`layer`, which the window sets). A Command names its layer and is live
// only while that layer has the keys, so a panel over another never shares
// a key with it.
//
// And how the window was last used, keyboard, mouse or touchpad (`input`),
// for the help to open on that one.
class Keymap : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString layer READ layer WRITE setLayer NOTIFY layerChanged)
    // The plugin over the photos (its id), one at a time: the "plugin"
    // layer's keys are its alone.
    Q_PROPERTY(QString plugin READ plugin WRITE setPlugin NOTIFY pluginChanged)
    Q_PROPERTY(QString input READ input NOTIFY inputChanged)
    // Counts up as actions come and go, for bindings over the list.
    Q_PROPERTY(int revision READ revision NOTIFY actionsChanged)

public:
    explicit Keymap(QObject *parent = nullptr);

    QString layer() const { return m_layer; }
    void setLayer(const QString &layer);
    QString plugin() const { return m_plugin; }
    void setPlugin(const QString &plugin);
    QString input() const { return m_input; }
    int revision() const { return m_revision; }

    // Command.qml adds itself as it is made, and goes as it is destroyed.
    Q_INVOKABLE void add(QObject *action);
    Q_INVOKABLE void remove(QObject *action);
    Q_INVOKABLE QVariantList actions() const;

    // Keys more than one live action answers to now, "Escape: help.close,
    // side.close": Qt then runs neither. Empty when all is well.
    Q_INVOKABLE QStringList clashes() const;
    // Actions some input cannot do, "grid.zoom-in: mouse", not counting
    // those that say why not (`noKeyboard`, `noMouse`, `noTouchpad`).
    Q_INVOKABLE QStringList uncovered() const;

signals:
    void layerChanged();
    void pluginChanged();
    void inputChanged();
    void actionsChanged();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void setInput(const QString &input);

    QString m_layer = QStringLiteral("grid");
    QString m_plugin;
    QString m_input = QStringLiteral("keyboard");
    // The pointer last used: a click says nothing of which, as Wayland
    // gives a touchpad's tap and a mouse's click alike; a scroll or a
    // gesture does.
    QString m_pointer = QStringLiteral("mouse");
    QList<QPointer<QObject>> m_actions;
    int m_revision = 0;
};
