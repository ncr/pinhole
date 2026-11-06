#include "outgoing.h"

#include "library.h"
#include "selftest.h"
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
#include <QStandardPaths>
#include <QTimer>

#include <memory>
#include <unistd.h>

Outgoing::Outgoing(const Library *library, ServiceClient *service, QObject *parent)
    : QObject(parent), m_library(library), m_service(service) {
    // Failed or partial copies are explained even when a button owns feedback.
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
    PromisedFiles(Outgoing *outgoing, Outgoing::Chosen items) : m_outgoing(outgoing), m_items(std::move(items)) {
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
            // The answer may come after the wait gave up (or never): it then
            // finds neither this loop nor this object, both perhaps gone by
            // then, only the state they shared, and leaves it be.
            struct Waiting {
                QEventLoop *loop = nullptr;
                QStringList files;
                bool done = false;
            };
            const auto waiting = std::make_shared<Waiting>();
            QEventLoop wait;
            waiting->loop = &wait;
            QTimer::singleShot(120000, &wait, &QEventLoop::quit);
            m_outgoing->prepare(m_items, true, {}, [waiting](const QStringList &files, int) {
                if (!waiting->loop)
                    return;
                waiting->files = files;
                waiting->done = true;
                waiting->loop->quit();
            });
            // Answered at once (all of them at hand): no wait, as a quit
            // before exec() would not end it.
            if (!waiting->done)
                wait.exec();
            waiting->loop = nullptr;
            m_files = waiting->files;
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
    Outgoing::Chosen m_items;
    bool m_started = false;
    mutable bool m_ready = false;
    mutable QStringList m_files;
};

} // namespace

Outgoing::Chosen Outgoing::snapshot(const QList<int> &indexes) const {
    Chosen items;
    items.source = m_library->source();
    for (int index : indexes) {
        if (index >= 0 && index < m_library->count())
            items.append(m_library->at(index));
    }
    return items;
}

QMimeData *Outgoing::dragData(const Chosen &items) { return new PromisedFiles(this, items); }

void Outgoing::drag(QObject *source, const QList<int> &indexes) {
    const Chosen items = snapshot(indexes);
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
    const Chosen items = snapshot(indexes);
    if (items.isEmpty())
        return;
    // Prepare asynchronously: clipboard managers ask for the bytes as soon
    // as the clipboard changes. Confirm only once those bytes are set.
    prepare(items, true, {}, [this, quiet](const QStringList &files, int failed) {
        if (files.isEmpty()) {
            emit copyFailed(0, failed);
            return;
        }
        toClipboard(files, this, [this, files, failed, quiet] {
            emit copied(int(files.size()), failed);
            if (failed)
                emit copyFailed(int(files.size()), failed);
            else if (!quiet)
                flash(QStringLiteral("✓ ") + tr("Copied %n photo(s)", "", int(files.size())));
        });
    }, quiet);
}

Outgoing::ClipboardImage Outgoing::clipboardImage(const QString &file) {
    QImageReader reader(file);
    reader.setAutoTransform(true);
    if (!reader.canRead())
        return {}; // a video
    ClipboardImage image;
    auto bytes = [&file] {
        QFile f(file);
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    };
    if (reader.format() == "jpeg")
        image.jpeg = bytes();
    // A GIF moves only as itself; its first frame as the PNG, for what
    // takes only that.
    if (reader.format() == "gif")
        image.gif = bytes();
    const QSize size = reader.size();
    if (size.isValid() && (size.width() > 2560 || size.height() > 2560))
        reader.setScaledSize(size.scaled(2560, 2560, Qt::KeepAspectRatio));
    const QImage picture = reader.read();
    if (!picture.isNull()) {
        QBuffer buffer(&image.png);
        buffer.open(QIODevice::WriteOnly);
        picture.save(&buffer, "PNG");
    }
    return image;
}

QMimeData *Outgoing::clipboardData(const QStringList &files, const ClipboardImage &image) {
    auto *mime = new QMimeData;
    QList<QUrl> urls;
    QByteArray gnome = "copy";
    for (const QString &f : files) {
        urls.append(QUrl::fromLocalFile(f));
        gnome += '\n' + QUrl::fromLocalFile(f).toEncoded();
    }
    mime->setUrls(urls);
    mime->setData(QStringLiteral("x-special/gnome-copied-files"), gnome);
    if (!image.jpeg.isEmpty())
        mime->setData(QStringLiteral("image/jpeg"), image.jpeg);
    if (!image.png.isEmpty())
        mime->setData(QStringLiteral("image/png"), image.png);
    if (!image.gif.isEmpty())
        mime->setData(QStringLiteral("image/gif"), image.gif);
    return mime;
}

void Outgoing::toClipboard(const QStringList &files, QObject *context, std::function<void()> then,
                           std::function<void()> dropped) {
    // Made here as finished bytes: left to Qt, a request for PNG (Omarchy's
    // clipboard history asks at once) encoded the full-size photo then and
    // there, and the window stood still for seconds.
    const QString single = files.size() == 1 ? files.first() : QString();
    // The last copy asked for wins: each is made on its own thread, and a
    // big photo's PNG asked for first can be ready after a small one asked
    // for later, which the clipboard must keep. (The window's thread only.)
    static quint64 latest = 0;
    const quint64 generation = ++latest;
    auto *watcher = new QFutureWatcher<ClipboardImage>(context);
    QObject::connect(watcher, &QFutureWatcherBase::finished, context, [watcher, files, then, dropped, generation] {
        watcher->deleteLater();
        if (generation != latest) {
            if (dropped)
                dropped();
            return;
        }
        setClipboard(clipboardData(files, watcher->result()));
        if (then)
            then();
    });
    watcher->setFuture(QtConcurrent::run([single] { return single.isEmpty() ? ClipboardImage() : clipboardImage(single); }));
}

void Outgoing::exportTo(const QList<int> &indexes, bool compatible) {
    const Chosen items = snapshot(indexes);
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

void Outgoing::prepare(const Chosen &items, bool compatible, const QString &folder, Done done, bool quiet) {
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
                         [this, item, source = items.source, compatible, target, finishOne](bool ok, const QJsonObject &result,
                                                                                             const QString &) {
                             if (!ok)
                                 return finishOne({});
                             place(result.value("path").toString(), result.value("type").toString(), item, source,
                                   compatible, target, finishOne);
                         }, false, items.source);
    }
}

void Outgoing::place(const QString &source, const QString &type, const LibraryItem &item, const QString &of,
                     bool compatible, const QString &folder, std::function<void(const QString &)> placed) {
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

    // A HEIC, which much that takes a photo does not read, as the service's
    // JPEG of the same full (`jpeg`): made once into its cache, under its
    // limit, with the date, place and camera kept and the pixels upright.
    if (compatible && type == QLatin1String("public.heic")) {
        const QString jpeg = QFileInfo(name).completeBaseName() + QStringLiteral(".jpg");
        m_service->fetch(item.id, QStringLiteral("jpeg"),
                         [folder, jpeg, hand, placed](bool ok, const QJsonObject &result, const QString &) {
                             if (!ok)
                                 return placed({});
                             hand(result.value("path").toString(), uniqueName(folder, jpeg));
                         }, false, of);
        return;
    }

    hand(source, uniqueName(folder, name));
}
