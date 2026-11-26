#pragma once

#include "adjustment.h"

#include <QHash>
#include <QObject>
#include <QString>
#include <QTimer>

#include <optional>

class AdjustmentStore;
class Library;
class ServiceClient;

// Edits on their way to a source that keeps them (iCloud: set_edit, as the
// iPhone writes its own). Each is drawn at once (AdjustmentStore::pending:
// a turn over the photo's files, which are of the edit the source has), and
// written only once the photo has been left alone for `quiet` ms: R pressed
// three times is one edit sent, of three quarters, and R then ⇧ R none. A
// cut, which cannot be drawn over files cut otherwise, is sent at once.
//
// Each is written over the edit the source has as far as this window
// knows: the one it wrote itself last, until the library read again shows
// it, else the library's (sourceEdit). An edit made elsewhere meanwhile is
// never written over (the service refuses it), and one refused or failed
// leaves the photo as the source has it, said through `failed`. Kept until
// the library read again has it, so the photo never shows the old files
// in the new shape. GUI thread.
class SourceEdits : public QObject {
    Q_OBJECT
public:
    SourceEdits(const Library *library, ServiceClient *service, AdjustmentStore *store, QObject *parent = nullptr);
    ~SourceEdits() override;

    // How long a photo is left alone before its edit is sent.
    void setQuiet(int ms) { m_timer.setInterval(ms); }
    int quiet() const { return m_timer.interval(); }

    // These photos of `source`'s library to be as `targets` say, asked by
    // the plugin `by` (its notices say what went wrong).
    void hold(const Adjustments &targets, const QString &source, const QString &by);
    // Its edit as it will be, if one is on its way.
    std::optional<Adjustment> target(const QByteArray &id) const;
    // All sent now, without waiting (the window closing).
    void flush();
    // Something still to send or being sent.
    bool busy() const;
    bool sending() const;
    // The library was read again: those it shows as edited are done.
    void libraryReloaded();

signals:
    // An edit not written: `by`'s, about `message`.
    void failed(const QString &by, const QString &message);
    // An edit written (for tests and the window closing).
    void wrote(const QByteArray &id);
    // Nothing left to send (busy no longer).
    void idle();

private:
    struct Edit {
        Adjustment target;
        // The last one written, until the library has it.
        std::optional<Adjustment> written;
        // Being sent: this one.
        std::optional<Adjustment> sending;
        QString source;
        QString by;
    };
    int positionOf(const QByteArray &id) const;
    Adjustment sourceEditOf(const QByteArray &id) const;
    void send(const QByteArray &id);
    void drop(const QByteArray &id);
    void checkIdle();

    const Library *m_library;
    ServiceClient *m_service;
    AdjustmentStore *m_store;
    QHash<QByteArray, Edit> m_edits;
    QTimer m_timer;
    // Those whose write is answered after this is gone see it so.
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};
