#pragma once

#include "library.h"

#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>

class ServiceClient;

// Files for photos leaving the app: dragged out, copied, exported. Each
// photo's original comes from pinholed and gets its camera filename; in the
// "compatible" form a HEIC becomes a JPEG (ImageMagick, which keeps the date,
// place and camera details), since much that accepts a dropped photo does not
// read HEIC. Videos always go as they are.
class Outgoing : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
public:
    Outgoing(const Library *library, ServiceClient *service, QObject *parent = nullptr);

    using Done = std::function<void(const QStringList &files, int failed)>;
    // The photos at these positions, as they are now. Everything that waits
    // (a download, the folder picker, a drag) carries these rather than
    // positions, which a library reload in the meantime would move.
    QList<LibraryItem> snapshot(const QList<int> &indexes) const;
    // Prepare the files, into `folder` (a staging folder if empty); `done`
    // gets them once all have arrived or failed.
    // `quiet`: no "Preparing…" status (the caller shows progress itself).
    void prepare(const QList<LibraryItem> &items, bool compatible, const QString &folder, Done done,
                 bool quiet = false);

    // What the window shows while files are on their way: "Preparing 3
    // photos…", "Exported 12 photos", and so on.
    QString status() const { return m_status; }
    void setStatus(const QString &status);

    static QString stagingFolder();

    // Drag these photos out of `source` (compatible form). The drag starts at
    // once; the files are handed over when the drop target asks for them,
    // waiting for any still downloading, like a file promise on a Mac.
    void drag(QObject *source, const QList<int> &indexes);
    // Put these photos on the clipboard (compatible form), as files, and for
    // a single photo as an image too, so pasting into a chat works.
    // `quiet`, from a button that shows its own tick: no status line; the
    // outcome comes as copied().
    Q_INVOKABLE void copy(const QList<int> &indexes, bool quiet = false);
    // Ask for a folder (the desktop's own picker) and export into it.
    Q_INVOKABLE void exportTo(const QList<int> &indexes, bool compatible);

signals:
    void statusChanged();
    // A copy finished: how many went on the clipboard (0: none could be
    // fetched) and how many could not.
    void copied(int files, int failed);

private:
    void flash(const QString &status);
    void place(const QString &source, const QString &type, const LibraryItem &item, bool compatible, const QString &folder,
               std::function<void(const QString &)> placed);
    static QString uniqueName(const QString &folder, const QString &name);

    const Library *m_library;
    ServiceClient *m_service;
    QString m_status;
};
