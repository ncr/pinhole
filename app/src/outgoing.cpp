#include "outgoing.h"

#include "library.h"
#include "portal.h"
#include "serviceclient.h"

#include <QDateTime>
#include <QClipboard>
#include <QtConcurrent/QtConcurrentRun>
#include <QImageReader>
#include <QFutureWatcher>
#include <QBuffer>
#include <QDir>
#include <QDrag>
#include <QEventLoop>
#include <QGuiApplication>
#include <QImage>
#include <QMimeData>
#include <QPixmap>
#include <QUrl>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>

#include <memory>
#include <unistd.h>

Outgoing::Outgoing(const Library *library, ServiceClient *service, QObject *parent)
    : QObject(parent), m_library(library), m_service(service) {
    // A copy is promised at once; should the files then not come, say so.
    connect(this, &Outgoing::copyFailed, this, [this](int files, int) {
        flash(files == 0 ? tr("Could not get the photos from %1").arg(m_service->sourceName())
                         : tr("Copied %n, some could not be fetched", "", files));
    });
    // Staged files from earlier days have long been dropped wherever they
    // were going.
    const QDateTime dayAgo = QDateTime::currentDateTime().addDays(-1);
    for (const QFileInfo &dir : QDir(stagingFolder()).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot))
        if (dir.lastModified() < dayAgo)
            QDir(dir.filePath()).removeRecursively();
}

void Outgoing::flash(const QString &status) {
    setStatus(status);
    QTimer::singleShot(3000, this, [this, status] {
        if (m_status == status)
            setStatus({});
    });
}

namespace {

// Files promised to a drop target: it sees text/uri-list straight away, and
// the list itself is produced when asked for, waiting for the files.
class PromisedFiles : public QMimeData {
public:
    PromisedFiles(Outgoing *outgoing, QList<LibraryItem> items) : m_outgoing(outgoing), m_items(std::move(items)) {
        // Under way once the drag's own event loop turns.
        QTimer::singleShot(0, this, [this] { m_started = true; });
    }

    QStringList formats() const override { return {QStringLiteral("text/uri-list")}; }
    bool hasFormat(const QString &mime) const override { return mime == QLatin1String("text/uri-list"); }

protected:
    QVariant retrieveData(const QString &mime, QMetaType type) const override {
        if (mime != QLatin1String("text/uri-list"))
            return {};
        // The files are made when a drop target asks for them. Qt's Wayland
        // drag also asks for the URL list as it starts (to offer a portal
        // file transfer), and waiting for downloads in there freed this
        // object under Qt and crashed: that gets nothing. It is told apart by
        // when it asks, not how: a drop target's request comes through
        // urls() as well (Qt 6.12), so an empty list there dropped nothing.
        if (!m_ready && !m_started)
            return QVariantList();
        if (!m_ready) {
            QEventLoop wait;
            QTimer::singleShot(120000, &wait, &QEventLoop::quit);
            m_outgoing->prepare(m_items, true, {}, [this, &wait](const QStringList &files, int) {
                m_files = files;
                wait.quit();
            });
            wait.exec();
            m_ready = true;
        }
        QList<QUrl> urls;
        for (const QString &f : m_files)
            urls.append(QUrl::fromLocalFile(f));
        if (type.id() == QMetaType::QByteArray || type.id() == QMetaType::QString) {
            QByteArray text;
            for (const QUrl &u : urls)
                text += u.toEncoded() + "\r\n";
            return text;
        }
        QVariantList list;
        for (const QUrl &u : urls)
            list.append(u);
        return list;
    }

private:
    Outgoing *m_outgoing;
    QList<LibraryItem> m_items;
    bool m_started = false;
    mutable bool m_ready = false;
    mutable QStringList m_files;
};

} // namespace

QList<LibraryItem> Outgoing::snapshot(const QList<int> &indexes) const {
    QList<LibraryItem> items;
    for (int index : indexes) {
        if (index >= 0 && index < m_library->count())
            items.append(m_library->at(index));
    }
    return items;
}

QMimeData *Outgoing::dragData(const QList<LibraryItem> &items) { return new PromisedFiles(this, items); }

void Outgoing::drag(QObject *source, const QList<int> &indexes) {
    const QList<LibraryItem> items = snapshot(indexes);
    if (items.isEmpty())
        return;
    auto *drag = new QDrag(source);
    drag->setMimeData(dragData(items));
    const QImage thumb(QFile::decodeName(items.first().thumbPath));
    if (!thumb.isNull())
        drag->setPixmap(QPixmap::fromImage(thumb.scaled(128, 128, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
    m_dragging = true;
    emit draggingChanged();
    drag->exec(Qt::CopyAction);
    m_dragging = false;
    emit draggingChanged();
}

void Outgoing::copy(const QList<int> &indexes, bool quiet) {
    const QList<LibraryItem> items = snapshot(indexes);
    if (items.isEmpty())
        return;
    // The button answers at once; the clipboard gets the files the moment
    // they are ready (at once, once a photo's JPEG is made). Not a promise
    // filled on demand: a clipboard manager (Omarchy's watches every copy)
    // asks straight away, and waiting for the files inside that request
    // stopped the window drawing until they came.
    emit copied(int(items.size()), 0);
    prepare(items, true, {}, [this, quiet](const QStringList &files, int failed) {
        if (files.isEmpty()) {
            emit copyFailed(0, failed);
            return;
        }
        // One photo is an image too, for pasting into chats: its JPEG as it
        // is, and a PNG of at most 2560 px for what takes only that. Made
        // here, away from the window's thread, as finished bytes: left to Qt,
        // a request for PNG (Omarchy's clipboard history asks at once)
        // encoded the full-size photo then and there, and the window stood
        // still for seconds.
        const QString single = files.size() == 1 ? files.first() : QString();
        auto *watcher = new QFutureWatcher<std::pair<QByteArray, QByteArray>>(this);
        connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, files, failed, quiet] {
            const auto [jpeg, png] = watcher->result();
            watcher->deleteLater();
            auto *mime = new QMimeData;
            QList<QUrl> urls;
            QByteArray gnome = "copy";
            for (const QString &f : files) {
                urls.append(QUrl::fromLocalFile(f));
                gnome += '\n' + QUrl::fromLocalFile(f).toEncoded();
            }
            mime->setUrls(urls);
            // File managers paste from this one.
            mime->setData(QStringLiteral("x-special/gnome-copied-files"), gnome);
            if (!jpeg.isEmpty())
                mime->setData(QStringLiteral("image/jpeg"), jpeg);
            if (!png.isEmpty())
                mime->setData(QStringLiteral("image/png"), png);
            QGuiApplication::clipboard()->setMimeData(mime);
            if (failed)
                emit copyFailed(int(files.size()), failed);
            else if (!quiet)
                flash(tr("Copied %n photo(s)", "", int(files.size())));
        });
        watcher->setFuture(QtConcurrent::run([single]() -> std::pair<QByteArray, QByteArray> {
            if (single.isEmpty())
                return {};
            QImageReader reader(single);
            reader.setAutoTransform(true);
            if (!reader.canRead())
                return {}; // a video
            QByteArray jpeg;
            if (reader.format() == "jpeg") {
                QFile file(single);
                if (file.open(QIODevice::ReadOnly))
                    jpeg = file.readAll();
            }
            const QSize size = reader.size();
            if (size.isValid() && (size.width() > 2560 || size.height() > 2560))
                reader.setScaledSize(size.scaled(2560, 2560, Qt::KeepAspectRatio));
            const QImage image = reader.read();
            QByteArray png;
            if (!image.isNull()) {
                QBuffer buffer(&png);
                buffer.open(QIODevice::WriteOnly);
                image.save(&buffer, "PNG");
            }
            return {jpeg, png};
        }));
    }, quiet);
}

void Outgoing::exportTo(const QList<int> &indexes, bool compatible) {
    const QList<LibraryItem> items = snapshot(indexes);
    if (items.isEmpty())
        return;
    auto *picker = new PortalFolderPicker(this);
    connect(picker, &PortalFolderPicker::picked, this, [this, picker, items, compatible](const QString &folder) {
        picker->deleteLater();
        if (folder.isEmpty())
            return;
        prepare(items, compatible, folder, [this, folder](const QStringList &files, int failed) {
            flash(failed ? tr("Exported %1, %2 failed").arg(files.size()).arg(failed)
                         : tr("Exported %n photo(s) to %1", "", int(files.size())).arg(QDir(folder).dirName()));
        });
    });
    picker->pick(tr("Export %n photo(s) to", "", int(items.size())));
}

void Outgoing::setStatus(const QString &status) {
    if (status == m_status)
        return;
    m_status = status;
    emit statusChanged();
}

QString Outgoing::stagingFolder() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + "/pinhole/outgoing";
}

QString Outgoing::uniqueName(const QString &folder, const QString &name) {
    const QFileInfo info(name);
    QString candidate = folder + '/' + name;
    for (int n = 2; QFileInfo::exists(candidate); ++n)
        candidate = QStringLiteral("%1/%2-%3.%4").arg(folder, info.completeBaseName()).arg(n).arg(info.suffix());
    return candidate;
}

void Outgoing::prepare(const QList<LibraryItem> &items, bool compatible, const QString &folder, Done done,
                       bool quiet) {
    QString target = folder;
    if (target.isEmpty()) {
        // A fresh staging folder per request: dropped files must not be
        // overwritten by the next drag while a target still reads them.
        target = stagingFolder() + '/' + QString::number(QDateTime::currentMSecsSinceEpoch());
    }
    QDir().mkpath(target);

    struct State {
        int left;
        int failed = 0;
        QStringList files;
        Done done;
    };
    auto state = std::make_shared<State>(State{int(items.size()), 0, {}, std::move(done)});
    if (items.isEmpty()) {
        state->done({}, 0);
        return;
    }
    if (!quiet)
        setStatus(items.size() == 1 ? tr("Preparing the photo…") : tr("Preparing %n photos…", "", int(items.size())));
    auto finishOne = [this, state, quiet](const QString &file) {
        if (file.isEmpty())
            state->failed += 1;
        else
            state->files.append(file);
        if (--state->left == 0) {
            if (!quiet)
                setStatus({});
            state->done(state->files, state->failed);
        }
    };
    for (const LibraryItem &item : items) {
        // The compatible form is the photo as it looks now (an edit, if it
        // has one); originals are as taken, unedited, as Photos exports
        // "unmodified originals".
        m_service->fetch(item.id, compatible ? QStringLiteral("full") : QStringLiteral("original"),
                         [this, item, compatible, target, finishOne](bool ok, const QJsonObject &result, const QString &) {
                             if (!ok)
                                 return finishOne({});
                             place(result.value("path").toString(), result.value("type").toString(), item, compatible,
                                   target, finishOne);
                         });
    }
}

void Outgoing::place(const QString &source, const QString &type, const LibraryItem &item, bool compatible,
                     const QString &folder, std::function<void(const QString &)> placed) {
    QString name = item.filename.isEmpty() ? QFileInfo(source).fileName() : item.filename;
    // An edit comes as a JPEG whatever the camera wrote: name it so.
    if (type == QLatin1String("public.jpeg") && !name.endsWith(QLatin1String(".jpg"), Qt::CaseInsensitive) &&
        !name.endsWith(QLatin1String(".jpeg"), Qt::CaseInsensitive))
        name = QFileInfo(name).completeBaseName() + QStringLiteral(".jpg");

    // A hard link where the cache and the target share a disk (free and
    // instant), a copy otherwise.
    auto hand = [placed](const QString &from, const QString &out) {
        if (::link(QFile::encodeName(from).constData(), QFile::encodeName(out).constData()) == 0 ||
            QFile::copy(from, out))
            placed(out);
        else
            placed({});
    };

    if (compatible && type == QLatin1String("public.heic")) {
        const QString out = uniqueName(folder, QFileInfo(name).completeBaseName() + ".jpg");
        // Converted once and kept in the cache beside the originals, named as
        // the original is (by its key, iCloud's checksum, so an edit converts anew);
        // pinholed's size limit covers it like the rest.
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + "/pinhole/compatible";
        const QString converted = dir + '/' + QFileInfo(source).completeBaseName() + ".jpg";
        if (QFileInfo::exists(converted)) {
            QFile file(converted);
            if (file.open(QIODevice::ReadWrite))
                file.setFileTime(QDateTime::currentDateTime(), QFileDevice::FileModificationTime); // recently used
            hand(converted, out);
            return;
        }
        QDir().mkpath(dir);
        // Written aside and renamed: two at once never clash, and a reader
        // never sees half a file. (pinholed's eviction skips .part files.)
        const QString part = converted + '.' + QString::number(QDateTime::currentMSecsSinceEpoch()) + ".part";
        auto *magick = new QProcess(this);
        connect(magick, &QProcess::finished, this,
                [magick, part, converted, out, hand, placed](int code, QProcess::ExitStatus status) {
                    magick->deleteLater();
                    if (status != QProcess::NormalExit || code != 0 || !QFileInfo::exists(part)) {
                        QFile::remove(part);
                        placed({});
                        return;
                    }
                    QFile::remove(converted);
                    QFile::rename(part, converted);
                    hand(converted, out);
                });
        magick->start(QStringLiteral("magick"),
                      {source, QStringLiteral("-quality"), QStringLiteral("92"), QStringLiteral("jpg:") + part});
        return;
    }

    hand(source, uniqueName(folder, name));
}
