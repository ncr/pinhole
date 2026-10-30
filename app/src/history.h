#pragma once

#include <QObject>
#include <QString>

#include <functional>
#include <vector>

// What was done in the window, to undo (Ctrl+Z) and do again (Ctrl+⇧Z):
// favourites, and whatever a plugin changes (pinhole.did, docs/plugins.md),
// as Series does with what it puts right by hand. Each step is its name and
// how to undo and redo it; a new one drops what was undone before it. It
// lasts while the window runs, and starts over with another source.
class History : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY changed)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY changed)
public:
    using Step = std::function<void()>;
    explicit History(QObject *parent = nullptr) : QObject(parent) {}

    bool canUndo() const { return m_done > 0; }
    bool canRedo() const { return m_done < int(m_steps.size()); }

    // Done just now: `name` says what, in a word or two ("Ungroup"). `owner`
    // is the plugin that did it, empty for Pinhole's own.
    void did(const QString &owner, const QString &name, Step undo, Step redo);
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    // A plugin turned off or loaded again: what it did can no longer be
    // undone by it.
    void forget(const QString &owner);
    void clear();

signals:
    void changed();
    // A step done just now, which the window offers to undo.
    void done(const QString &name);
    void undone(const QString &name);
    void redone(const QString &name);
    // Ctrl+Z with nothing to undo, or Ctrl+⇧Z with nothing to redo.
    void nothing(bool undo);

private:
    struct Entry {
        QString owner, name;
        Step undo, redo;
    };
    std::vector<Entry> m_steps;
    // How many of them are done; those after were undone.
    int m_done = 0;
    // Undoing or redoing: what that does is not a step of its own.
    bool m_replaying = false;
};
