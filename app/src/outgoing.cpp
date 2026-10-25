#include "outgoing.h"

#include "library.h"
#include "portal.h"
#include "serviceclient.h"

#include <QDateTime>
#include <QClipboard>
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
    PromisedFiles(Outgoing *outgoing, QList<LibraryItem> items) : m_outgoing(outgoing), m_items(std::move(items)) {}

    QStringList formats() const override { return {QStringLiteral("text/uri-list")}; }
    bool hasFormat(const QString &mime) const override { return mime == QLatin1String("text/uri-list"); }

protected:
    QVariant retrieveData(const QString &mime, QMetaType type) const override {
        if (mime != QLatin1String("text/uri-list"))
            return {};
        // The files are made when a drop target asks for the bytes. Asking
        // for the URL list is something else: Qt's Wayland drag does it as
        // the drag starts (to offer a portal file transfer), and waiting for
        // downloads in there freed this object under Qt and crashed. That
        // gets nothing until the files exist.
        const bool bytes = type.id() == QMetaType::QByteArray || type.id() == QMetaType::QString;
        if (!m_ready && !bytes)
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

void Outgoing::drag(QObject *source, const QList<int> &indexes) {
    const QList<LibraryItem> items = snapshot(indexes);
    if (items.isEmpty())
        return;
    auto *drag = new QDrag(source);
    drag->setMimeData(new PromisedFiles(this, items));
    const QImage thumb(QFile::decodeName(items.first().thumbPath));
    if (!thumb.isNull())
        drag->setPixmap(QPixmap::fromImage(thumb.scaled(128, 128, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
    drag->exec(Qt::CopyAction);
}

void Outgoing::copy(const QList<int> &indexes) {
    const QList<LibraryItem> items = snapshot(indexes);
    if (items.isEmpty())
        return;
    prepare(items, true, {}, [this](const QStringList &files, int failed) {
        if (files.isEmpty()) {
            flash(tr("Could not get the photos from iCloud"));
            return;
        }
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
        if (files.size() == 1) {
            const QImage image(files.first());
            if (!image.isNull())
                mime->setImageData(image);
        }
        QGuiApplication::clipboard()->setMimeData(mime);
        flash(failed ? tr("Copied %n, some could not be fetched", "", int(files.size()))
                     : tr("Copied %n photo(s)", "", int(files.size())));
    });
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

void Outgoing::prepare(const QList<LibraryItem> &items, bool compatible, const QString &folder, Done done) {
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
    setStatus(items.size() == 1 ? tr("Preparing the photo…") : tr("Preparing %n photos…", "", int(items.size())));
    auto finishOne = [this, state](const QString &file) {
        if (file.isEmpty())
            state->failed += 1;
        else
            state->files.append(file);
        if (--state->left == 0) {
            setStatus({});
            state->done(state->files, state->failed);
        }
    };
    for (const LibraryItem &item : items) {
        m_service->fetch(item.id, QStringLiteral("original"),
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

    if (compatible && type == QLatin1String("public.heic")) {
        const QString out = uniqueName(folder, QFileInfo(name).completeBaseName() + ".jpg");
        auto *magick = new QProcess(this);
        connect(magick, &QProcess::finished, this, [magick, out, placed](int code, QProcess::ExitStatus status) {
            magick->deleteLater();
            placed(status == QProcess::NormalExit && code == 0 && QFileInfo::exists(out) ? out : QString());
        });
        magick->start(QStringLiteral("magick"), {source, QStringLiteral("-quality"), QStringLiteral("92"), out});
        return;
    }

    // As is: a hard link where the cache and the target share a disk (free
    // and instant), a copy otherwise.
    const QString out = uniqueName(folder, name);
    if (::link(QFile::encodeName(source).constData(), QFile::encodeName(out).constData()) == 0 ||
        QFile::copy(source, out)) {
        placed(out);
    } else {
        placed({});
    }
}
