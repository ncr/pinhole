#pragma once

#include "library.h"

#include <QList>
#include <QObject>

#include <functional>

class ServiceClient;

// Photos made favourites, or not: what pinhole.setFavorite does
// (docs/plugins.md), for the Favourites plugin (plugins/favourites) and
// any other. The library in memory changes at once, so the grid's mark
// and the viewer show it, while pinholed sets it in the source and the
// index (docs/service.md, Favourites); the library read again afterwards
// has it from there. What the service could not set goes back as it was.
// Undo is the plugin's own (pinhole.did).
class Favourites : public QObject {
    Q_OBJECT
    // Changes with any favourite in memory, here or by a reload.
    Q_PROPERTY(int revision READ revision NOTIFY changed)
public:
    Favourites(Library *library, ServiceClient *service, QObject *parent = nullptr);

    int revision() const { return m_revision; }
    // The service's answer: `code` empty once they are set, else its code
    // and message, the photos put back as they were.
    using Done = std::function<void(const QString &code, const QString &message)>;
    // These photos (ids of the library shown) favourites, or not: those
    // not so already, which it returns, at once in memory and then in the
    // source. `done` is called when the service answers, or at once when
    // there is nothing to change.
    QList<QByteArray> set(const QList<QByteArray> &ids, bool favorite, const Done &done);
    // The library was read again.
    void reloaded();

signals:
    void changed();

private:
    // Set these in memory, wherever they are now.
    void apply(const QList<QByteArray> &ids, bool favorite);

    Library *m_library;
    ServiceClient *m_service;
    int m_revision = 0;
};
