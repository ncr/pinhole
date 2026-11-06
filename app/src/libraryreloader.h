#pragma once

#include <QObject>
#include <QString>
#include <QTimer>

#include <memory>
#include <thread>
#include <vector>

class Library;
class ThumbnailLoader;

// Reads the library index again when the service says it changed, and swaps
// it in: the reading, and matching old positions to new (~100 ms at 100,000
// photos), on a thread of its own; the swap on this one, with the decoders
// idle (ThumbnailLoader::reload). A change announced while one is read
// reads once more after it. Everything that holds positions follows its
// photos through `reloaded`. A folder with no library in it (library/none,
// while the service shows no source) is an empty library, never the last
// one's photos kept on screen. GUI thread.
class LibraryReloader : public QObject {
    Q_OBJECT
public:
    LibraryReloader(Library *library, ThumbnailLoader *loader, const QString &dir, QObject *parent = nullptr);
    ~LibraryReloader() override;

    // Where the library is read from; the service may move it.
    QString dir() const { return m_dir; }
    void moveTo(const QString &dir);

    void reload();
    // Where what was done to the photos is kept: followed into each read,
    // and a change there reads the library again (adjustment.h).
    void setAdjustments(class AdjustmentStore *store);
    // The next reload is another source's library: shown from its newest
    // photos, not from where the last one was (see `reloaded`).
    void showNewestNext() { m_toNewest = true; }
    // No library yet: look for one every few seconds (the service may be
    // making it) and read it once it is there.
    void waitForLibrary();

signals:
    // Just before the swap, while positions still mean the old photos.
    void aboutToSwap();
    // Just after it: `moved[old]` is each photo's new position or -1,
    // `textures` the same unless a photo's thumbnail is a new file (then
    // -1: its old one must not stay on screen). `toNewest` as in
    // showNewestNext. Delivered directly, on this thread.
    void reloaded(const std::vector<int> &moved, const std::vector<int> &textures, bool toNewest);
    // After `reloaded`, when what was swapped in is another source's
    // library, however it came to be shown (this window, another, a
    // terminal): nothing done to the last one's photos is to be undone on
    // these, though a photo here has the same id.
    void otherSource();

private:
    struct Read;
    void swapIn(const std::shared_ptr<Read> &read);

    Library *m_library;
    ThumbnailLoader *m_loader;
    QString m_dir;
    class AdjustmentStore *m_adjustments = nullptr;
    std::thread m_reader;
    bool m_reading = false;
    bool m_again = false;
    bool m_toNewest = false;
    QTimer m_wait;
};
