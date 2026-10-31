#pragma once

#include "library.h"

#include <QList>
#include <QObject>

class History;
class ServiceClient;

// Photos made favourites, or not, from the window. The library in memory
// changes on the press, so the grid's mark and the viewer's ♥ show it at
// once, while pinholed sets it in the source and the index
// (docs/service.md, Favourites); the library read again afterwards has it
// from there. What the service could not set goes back as it was. Each
// press is a step of the history, to undo.
class Favourites : public QObject {
    Q_OBJECT
    // Changes with any favourite in memory, here or by a reload: for
    // bindings over all().
    Q_PROPERTY(int revision READ revision NOTIFY changed)
public:
    Favourites(Library *library, ServiceClient *service, History *history, QObject *parent = nullptr);

    int revision() const { return m_revision; }
    // Whether the photos at these positions are all favourites (and there
    // are some).
    Q_INVOKABLE bool all(const QList<int> &indexes) const;
    // All of them favourites, or none when they all are, as in Photos.
    Q_INVOKABLE void toggle(const QList<int> &indexes);
    // The library was read again.
    void reloaded();

signals:
    void changed();
    // The service could not set them, in a few words.
    void failed(const QString &message);

private:
    // Set these here and in the source, those of `source`'s library (as
    // Library::source says): of no other's, though it has the same ids.
    void set(const QList<QByteArray> &ids, bool favorite, const QString &source);
    // Set these in memory, wherever they are now.
    void apply(const QList<QByteArray> &ids, bool favorite);

    Library *m_library;
    ServiceClient *m_service;
    History *m_history;
    int m_revision = 0;
};
