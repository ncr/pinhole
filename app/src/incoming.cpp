#include "incoming.h"

#include "outgoing.h"
#include "serviceclient.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>

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

bool Incoming::ownCopy(const QString &path) {
    const QString own = QDir::cleanPath(Outgoing::stagingFolder()) + '/';
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath()).startsWith(own);
}

QStringList Incoming::accepts() const {
    QStringList takes;
    for (const QJsonValue &v : m_service->source().value("accepts").toArray())
        takes.append(v.toString());
    return takes;
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
        if (takes.isEmpty() || !url.isLocalFile() || ownCopy(url.toLocalFile()))
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
        if (url.isLocalFile() && !ownCopy(url.toLocalFile()))
            paths.append(QFileInfo(url.toLocalFile()).absoluteFilePath());
    }
    if (paths.isEmpty())
        return;
    // For the source shown when they were dropped: one switched to while
    // they wait their turn is not where they go (the service refuses).
    const Drop drop{paths, m_service->source().value("id").toString(), place()};
    if (m_busy) {
        m_queue.append(drop);
        return;
    }
    start(drop);
}

void Incoming::start(const Drop &drop) {
    m_problems.clear();
    m_failed = 0;
    m_busy = true;
    m_progress = 0;
    m_place = drop.place;
    // Files dropped as they are; a folder's are not counted here (a big
    // folder on a slow disk must not hold the window still before anything
    // is sent), and the service counts assets, a Live Photo's two files as
    // one, where "Added" counts files: the folder is said as photos and
    // videos until they are in.
    const QStringList takes = accepts();
    m_files = 0;
    bool folders = false;
    for (const QString &path : drop.paths) {
        const QFileInfo info(path);
        if (info.isDir())
            folders = true;
        else if (takes.contains(info.suffix().toLower()))
            ++m_files;
    }
    m_status = folders ? tr("Adding the photos and videos to %1…").arg(m_place)
                         : tr("Adding %n file(s) to %1…", "", std::max(1, m_files)).arg(m_place);
    m_fade.stop();
    emit changed();
    m_service->call(QStringLiteral("add"), {{"files", QJsonArray::fromStringList(drop.paths)}, {"source", drop.source}},
                    [this](bool ok, const QJsonObject &result, const QString &code) {
                        if (ok) {
                            finished(result);
                            return;
                        }
                        m_problems = {reason(code, {})};
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
            m_problems.append(tr("%1: already in %2").arg(name, m_place));
        } else {
            m_failed += files;
            const QJsonObject error = file.value("error").toObject();
            m_problems.append(QStringLiteral("%1: %2").arg(name, reason(error.value("code").toString(), error.value("message").toString())));
        }
    }
    m_busy = false;
    m_progress = 1;
    m_status = added > 0 ? tr("Added %n file(s) to %1", "", added).arg(m_place)
             : duplicates > 0 && m_failed == 0 ? tr("%1 has them already").arg(m_place)
                                               : QString();
    m_fade.start();
    emit changed();
    if (!m_queue.isEmpty()) {
        const QStringList before = m_problems;
        const int failed = m_failed;
        start(m_queue.takeFirst());
        // The problems of the drop before stay in view with these.
        m_problems = before + m_problems;
        m_failed += failed;
        emit changed();
    }
}

QString Incoming::reason(const QString &code, const QString &message) const {
    if (code == QLatin1String("other_source"))
        return tr("%1 is no longer the library shown, so nothing was added to it").arg(m_place);
    if (code == QLatin1String("unsupported"))
        return tr("not a photo or a video that %1 takes").arg(m_place);
    if (code == QLatin1String("too_large"))
        return tr("too large for %1").arg(m_place);
    if (code == QLatin1String("over_quota"))
        return tr("%1 storage is full").arg(m_place);
    if (code == QLatin1String("protected"))
        return tr("Advanced Data Protection keeps iCloud.com out of the library, so nothing can be added from here");
    if (code == QLatin1String("needs_sign_in"))
        return tr("not signed in to %1").arg(m_place);
    if (code == QLatin1String("unavailable"))
        return tr("%1 could not be reached").arg(m_place);
    if (code == QLatin1String("read_only"))
        return tr("this library is only being looked at here: nothing can be added to it");
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
