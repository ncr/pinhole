#include "incoming.h"

#include "serviceclient.h"

#include <QDirIterator>
#include <QFileInfo>
#include <QJsonArray>

namespace {

// As core/src/add.rs and icloud/upload.rs have them, for a service from
// before it said.
const QStringList folderTakes{"jpg", "jpeg", "heic", "heif", "png", "gif", "tif", "tiff", "webp", "avif",
                              "mov", "mp4", "m4v", "mkv", "webm", "avi"};
const QStringList icloudTakes{"jpg", "jpeg", "png", "heic", "heif", "tif", "tiff", "gif", "webp", "mov", "mp4", "m4v"};

// A Live Photo's motion counts as a file of its own here, as the person
// dropped it; the service pairs it with its photo.
int filesIn(const QStringList &paths, const QStringList &takes) {
    int n = 0;
    for (const QString &path : paths) {
        const QFileInfo info(path);
        if (info.isDir()) {
            QDirIterator it(path, QDir::Files, QDirIterator::Subdirectories);
            while (it.hasNext())
                n += takes.contains(QFileInfo(it.next()).suffix().toLower()) ? 1 : 0;
        } else if (takes.contains(info.suffix().toLower())) {
            ++n;
        }
    }
    return n;
}

} // namespace

Incoming::Incoming(ServiceClient *service, QObject *parent) : QObject(parent), m_service(service) {
    connect(service, &ServiceClient::announced, this, &Incoming::heard);
    // "Added…" stays a few seconds; what failed stays until dismissed.
    m_fade.setSingleShot(true);
    m_fade.setInterval(4000);
    connect(&m_fade, &QTimer::timeout, this, [this] {
        if (m_busy)
            return;
        m_status.clear();
        emit changed();
    });
}

QStringList Incoming::accepts() const {
    const QJsonObject source = m_service->source();
    if (source.contains("accepts")) {
        QStringList takes;
        for (const QJsonValue &v : source.value("accepts").toArray())
            takes.append(v.toString());
        return takes;
    }
    const QString kind = source.value("kind").toString();
    return kind == QLatin1String("icloud") ? icloudTakes : kind == QLatin1String("folder") ? folderTakes : QStringList();
}

QString Incoming::place() const {
    const QJsonObject source = m_service->source();
    // An Apple ID's source is named after its owner, which reads wrong here.
    return source.value("kind").toString() == QLatin1String("icloud") ? QStringLiteral("iCloud")
                                                                        : source.value("name").toString();
}

QVariantMap Incoming::wanted(const QList<QUrl> &urls) const {
    const QStringList takes = accepts();
    int files = 0, folders = 0;
    for (const QUrl &url : urls) {
        if (takes.isEmpty() || !url.isLocalFile())
            continue;
        const QFileInfo info(url.toLocalFile());
        if (info.isDir())
            ++folders;
        else if (takes.contains(info.suffix().toLower()))
            ++files;
    }
    return {{"files", files}, {"folders", folders}};
}

void Incoming::add(const QList<QUrl> &urls) {
    QStringList paths;
    for (const QUrl &url : urls) {
        if (url.isLocalFile())
            paths.append(QFileInfo(url.toLocalFile()).absoluteFilePath());
    }
    if (paths.isEmpty())
        return;
    if (m_busy) {
        m_queue.append(paths);
        return;
    }
    m_problems.clear();
    m_failed = 0;
    m_busy = true;
    m_progress = 0;
    m_files = filesIn(paths, accepts());
    m_status = tr("Adding %n file(s) to %1…", "", std::max(1, m_files)).arg(place());
    m_fade.stop();
    emit changed();
    m_service->call(QStringLiteral("add"), {{"files", QJsonArray::fromStringList(paths)}},
                    [this](bool ok, const QJsonObject &result, const QString &code) {
                        if (ok) {
                            finished(result);
                            return;
                        }
                        m_problems = {code == QLatin1String("bad_request")
                                          ? tr("The Pinhole service running now is older than this window and has to be restarted.")
                                          : reason(code, {})};
                        m_failed = std::max(1, m_files);
                        finished({});
                    });
}

void Incoming::heard(const QString &name, const QJsonObject &details) {
    if (name != QLatin1String("adding") || !m_busy)
        return;
    const double bytes = details.value("bytes").toDouble();
    m_progress = bytes > 0 ? details.value("sent").toDouble() / bytes : 0;
    emit changed();
}

void Incoming::finished(const QJsonObject &result) {
    int added = 0, duplicates = 0;
    for (const QJsonValue &v : result.value("files").toArray()) {
        const QJsonObject file = v.toObject();
        const QString state = file.value("state").toString();
        const QString name = QFileInfo(file.value("file").toString()).fileName();
        const int files = std::max<int>(1, file.value("files").toArray().size());
        if (state == QLatin1String("added")) {
            added += files;
            if (file.contains("note"))
                m_problems.append(tr("%1: added as a still photo, since iCloud on the web does not take a Live Photo's motion").arg(name));
        } else if (state == QLatin1String("duplicate")) {
            duplicates += files;
            m_problems.append(tr("%1: already in %2").arg(name, place()));
        } else {
            m_failed += files;
            const QJsonObject error = file.value("error").toObject();
            m_problems.append(QStringLiteral("%1: %2").arg(name, reason(error.value("code").toString(), error.value("message").toString())));
        }
    }
    m_busy = false;
    m_progress = 1;
    m_status = added > 0 ? tr("Added %n file(s) to %1", "", added).arg(place())
             : duplicates > 0 && m_failed == 0 ? tr("%1 has them already").arg(place())
                                               : QString();
    m_fade.start();
    emit changed();
    if (!m_queue.isEmpty()) {
        const QStringList next = m_queue.takeFirst();
        QList<QUrl> urls;
        for (const QString &p : next)
            urls.append(QUrl::fromLocalFile(p));
        const QStringList before = m_problems;
        const int failed = m_failed;
        add(urls);
        // The problems of the drop before stay in view with these.
        m_problems = before + m_problems;
        m_failed += failed;
        emit changed();
    }
}

QString Incoming::reason(const QString &code, const QString &message) const {
    if (code == QLatin1String("unsupported"))
        return tr("not a photo or a video %1 takes").arg(place());
    if (code == QLatin1String("too_large"))
        return tr("too large for %1").arg(place());
    if (code == QLatin1String("over_quota"))
        return tr("%1 storage is full").arg(place());
    if (code == QLatin1String("protected"))
        return tr("Advanced Data Protection keeps iCloud.com out of the library, so nothing can be added from here");
    if (code == QLatin1String("needs_sign_in"))
        return tr("not signed in to %1").arg(place());
    if (code == QLatin1String("unavailable"))
        return tr("%1 could not be reached").arg(place());
    return message.isEmpty() ? tr("could not be added") : message;
}

QString Incoming::problemsTitle() const {
    if (m_failed > 0)
        return tr("%n file(s) not added", "", m_failed);
    return m_problems.isEmpty() ? QString() : tr("Added, with a note");
}

void Incoming::dismiss() {
    if (m_busy)
        return;
    m_problems.clear();
    m_failed = 0;
    m_status.clear();
    emit changed();
}
