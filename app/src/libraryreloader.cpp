#include "libraryreloader.h"

#include "library.h"
#include "thumbnailloader.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>

#include <cstdio>

namespace {

// Which source's library a folder is (library/<kind>/<name>): the same one
// moved elsewhere keeps it, another source's has another.
QString sourceOf(const QString &dir) {
    const QStringList parts = dir.split('/', Qt::SkipEmptyParts);
    return parts.mid(std::max(0, int(parts.size()) - 2)).join('/');
}

} // namespace

// One read of the index, and how its positions follow the library shown.
struct LibraryReloader::Read {
    // The folder it was read from.
    QString dir;
    Library fresh;
    std::vector<int> moved, textures;
    // The same source's library as the one shown.
    bool same = true;
    // No library.sqlite there: read as an empty one.
    bool missing = false;
    qint64 readMs = 0;
};

LibraryReloader::LibraryReloader(Library *library, ThumbnailLoader *loader, const QString &dir, QObject *parent)
    : QObject(parent), m_library(library), m_loader(loader), m_dir(QDir(dir).absolutePath()) {
    m_wait.setInterval(5000);
    connect(&m_wait, &QTimer::timeout, this, [this] {
        if (!QFile::exists(m_dir + "/library.sqlite"))
            return;
        m_wait.stop();
        reload();
    });
}

LibraryReloader::~LibraryReloader() {
    // The reader is done once joined, and what it posted here goes with
    // this object.
    if (m_reader.joinable())
        m_reader.join();
}

void LibraryReloader::moveTo(const QString &dir) {
    // Said again as a window connects: read already.
    if (QDir(dir).absolutePath() == m_dir)
        return;
    m_dir = QDir(dir).absolutePath();
    reload();
}

void LibraryReloader::waitForLibrary() {
    m_wait.start();
}

void LibraryReloader::reload() {
    if (m_reading) {
        m_again = true;
        return;
    }
    m_reading = true;
    if (m_reader.joinable())
        m_reader.join();
    // Only this thread swaps the library, and not before the reader hands
    // over, so the reader may read the one shown meanwhile.
    m_reader = std::thread([this, dir = m_dir, library = m_library] {
        QElapsedTimer took;
        took.start();
        auto read = std::make_shared<Read>();
        read->dir = dir;
        QString error;
        // None there is an empty one, not the last one's photos kept: the
        // service shows nothing (library/none, its sources' plugins off, or
        // none added) or has not made it yet. It is read once it is there.
        read->missing = !QFile::exists(dir + "/library.sqlite");
        if (!read->fresh.open(dir, &error) && !read->missing) {
            std::fprintf(stderr, "pinhole: reloading the library: %s\n", qPrintable(error));
            QMetaObject::invokeMethod(this, [this] {
                m_reading = false;
                if (std::exchange(m_again, false))
                    reload();
            }, Qt::QueuedConnection);
            return;
        }
        const Library &fresh = read->fresh;
        QHash<QByteArray, int> positions;
        positions.reserve(fresh.count());
        for (int i = 0; i < fresh.count(); ++i)
            positions.insert(fresh.at(i).id, i);
        read->moved.assign(size_t(library->count()), -1);
        // Another source's library: nothing carries over, though a photo
        // there have the same id (a folder's are paths). By what each says
        // it is, else by where it is.
        const bool same = !fresh.source().isEmpty() && !library->source().isEmpty()
                              ? fresh.source() == library->source()
                              : sourceOf(fresh.dir()) == sourceOf(library->dir());
        read->same = same;
        if (same)
            for (int i = 0; i < library->count(); ++i)
                read->moved[size_t(i)] = positions.value(library->at(i).id, -1);
        // A photo whose thumbnail is a new file is drawn afresh.
        read->textures = read->moved;
        for (int i = 0; i < library->count(); ++i)
            if (const int now = read->moved[size_t(i)]; now >= 0 && fresh.at(now).thumbPath != library->at(i).thumbPath)
                read->textures[size_t(i)] = -1;
        read->readMs = took.elapsed();
        QMetaObject::invokeMethod(this, [this, read] { swapIn(read); }, Qt::QueuedConnection);
    });
}

void LibraryReloader::swapIn(const std::shared_ptr<Read> &read) {
    // Read from where the library was, before it moved or another was
    // shown: never swapped in; the one there now is read instead.
    if (read->dir != m_dir) {
        m_reading = false;
        m_again = false;
        reload();
        return;
    }
    QElapsedTimer swapping;
    swapping.start();
    const int before = m_library->count();
    emit aboutToSwap();
    m_loader->reload([&] { m_library->swap(read->fresh); });
    emit reloaded(read->moved, read->textures, std::exchange(m_toNewest, false));
    if (!read->same)
        emit otherSource();
    if (read->missing)
        m_wait.start();
    else
        m_wait.stop();
    std::fprintf(stderr, "pinhole: library reloaded: %d items (was %d); read in %lld ms, swapped in %lld ms\n",
                 m_library->count(), before, read->readMs, swapping.elapsed());
    m_reading = false;
    if (std::exchange(m_again, false))
        reload();
}
