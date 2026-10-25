#include "viewer.h"

#include "library.h"
#include "photogrid.h"
#include "serviceclient.h"

#include <QDateTime>
#include <QFile>
#include <QLocale>
#include <QWheelEvent>

#include <cmath>

Viewer::Viewer(const Library *library, ServiceClient *service, QObject *parent)
    : QObject(parent), m_library(library), m_service(service) {}

QUrl Viewer::providerUrl(const QString &path) {
    return QUrl(QStringLiteral("image://photo/") + QString::fromUtf8(QUrl::toPercentEncoding(path)));
}

QUrl Viewer::thumbnail() const {
    if (m_index < 0 || m_index >= m_library->count())
        return {};
    return QUrl::fromLocalFile(QFile::decodeName(m_library->at(m_index).thumbPath));
}

qreal Viewer::aspect() const {
    if (m_index < 0 || m_index >= m_library->count())
        return 1;
    const LibraryItem &item = m_library->at(m_index);
    return item.thumbH ? qreal(item.thumbW) / item.thumbH : 1;
}

bool Viewer::isVideo() const { return m_index >= 0 && m_index < m_library->count() && m_library->at(m_index).video; }
bool Viewer::isLive() const { return m_index >= 0 && m_index < m_library->count() && m_library->at(m_index).live; }
bool Viewer::isFavorite() const {
    return m_index >= 0 && m_index < m_library->count() && m_library->at(m_index).favorite;
}

QString Viewer::title() const {
    if (m_index < 0 || m_index >= m_library->count())
        return {};
    const QDateTime when = QDateTime::fromMSecsSinceEpoch(m_library->at(m_index).date);
    // "Saturday, 24 October 2026  16:16", in the locale's own words.
    const QLocale locale;
    return locale.toString(when.date(), QLocale::LongFormat) + QStringLiteral("  ") +
           locale.toString(when.time(), QLocale::ShortFormat);
}

void Viewer::setMessage(const QString &message) {
    if (message == m_message)
        return;
    m_message = message;
    emit messageChanged();
}

void Viewer::openAt(int index, QRectF from) {
    if (index < 0 || index >= m_library->count())
        return;
    m_origin = from;
    show(index);
    if (!m_open) {
        m_open = true;
        emit openChanged();
    }
}

bool Viewer::eventFilter(QObject *, QEvent *event) {
    if (event->type() != QEvent::Wheel || !m_open)
        return false;
    auto *wheel = static_cast<QWheelEvent *>(event);
    const QPoint px = wheel->pixelDelta();
    const QPoint angle = wheel->angleDelta();
    logInput(QStringLiteral("viewer wheel phase=%1 pixel=%2,%3 angle=%4,%5 inverted=%6 ts=%7")
                 .arg(int(wheel->phase())).arg(px.x()).arg(px.y()).arg(angle.x()).arg(angle.y())
                 .arg(wheel->inverted()).arg(wheel->timestamp()));
    if (wheel->modifiers() & Qt::ControlModifier)
        return true;

    if (wheel->phase() == Qt::NoScrollPhase && px.isNull()) {
        // A mouse wheel: a notch (120) is a photo, down for the next.
        m_notches += (angle.y() != 0 ? angle.y() : angle.x()) / 120.0;
        for (; m_notches <= -1; m_notches += 1)
            step(1);
        for (; m_notches >= 1; m_notches -= 1)
            step(-1);
        return true;
    }

    // A touchpad: one step per swipe, once it has gone far enough and
    // mostly sideways. Fingers moving left bring the next photo, as on a
    // Mac. Without phases, a pause of 300 ms starts a new swipe.
    const quint64 t = wheel->timestamp();
    if (wheel->phase() == Qt::ScrollBegin || t - m_lastWheel > 300) {
        m_swipe = QPointF();
        m_swiped = false;
    }
    m_lastWheel = t;
    m_swipe += px.isNull() ? QPointF(angle) / 8.0 : QPointF(px);
    if (!m_swiped && std::abs(m_swipe.x()) > 40 && std::abs(m_swipe.x()) > 1.5 * std::abs(m_swipe.y())) {
        m_swiped = true;
        step(m_swipe.x() < 0 ? 1 : -1);
    }
    return true;
}

void Viewer::close() {
    if (!m_open)
        return;
    m_open = false;
    m_generation += 1; // anything still arriving is for a closed viewer
    emit openChanged();
}

void Viewer::step(int delta) {
    const int next = std::clamp(m_index + delta, 0, m_library->count() - 1);
    if (next != m_index)
        show(next);
}

void Viewer::show(int index) {
    m_generation += 1;
    m_index = index;
    m_image = QUrl();
    m_stage = QStringLiteral("thumbnail");
    m_video = QUrl();
    m_live = QUrl();
    setMessage({});
    emit indexChanged();
    emit imageChanged();
    emit videoChanged();
    emit liveChanged();

    if (isVideo()) {
        setMessage(tr("Loading video…"));
        request(index, QStringLiteral("video"));
    } else {
        request(index, QStringLiteral("medium"));
    }
    // The neighbours, so stepping through is instant.
    for (int d : {1, -1}) {
        const int n = index + d;
        if (n >= 0 && n < m_library->count() && !m_library->at(n).video)
            m_service->fetch(m_library->at(n).id, QStringLiteral("medium"), [](bool, const QJsonObject &, const QString &) {});
    }
}

void Viewer::request(int index, const QString &kind) {
    const int generation = m_generation;
    m_service->fetch(m_library->at(index).id, kind, [this, generation, index, kind](bool ok, const QJsonObject &result,
                                                                                    const QString &code) {
        if (generation != m_generation)
            return;
        if (!ok) {
            // No medium rendition (some old photos): go straight to the
            // original. Anything else is reported, and what is shown stays.
            if (kind == QLatin1String("medium") && code == QLatin1String("not_found")) {
                request(index, QStringLiteral("original"));
                return;
            }
            if (kind != QLatin1String("live"))
                setMessage(code == QLatin1String("needs_sign_in") ? tr("Not signed in to iCloud")
                                                                  : tr("Could not load from iCloud"));
            return;
        }
        const QString path = result.value("path").toString();
        // A video still downloading plays from the service as it arrives.
        const QString stream = result.value("stream").toString();
        const QUrl playable = stream.isEmpty() ? QUrl::fromLocalFile(path) : QUrl(stream);
        if (kind == QLatin1String("video")) {
            m_video = playable;
            setMessage({});
            emit videoChanged();
        } else if (kind == QLatin1String("live")) {
            m_live = playable;
            emit liveChanged();
        } else {
            m_image = providerUrl(path);
            m_stage = kind;
            emit imageChanged();
            // The medium is on screen; the original follows.
            if (kind == QLatin1String("medium"))
                request(index, QStringLiteral("original"));
        }
    }, kind == QLatin1String("video") || kind == QLatin1String("live"));
}

void Viewer::playLive() {
    if (!isLive())
        return;
    if (!m_live.isEmpty()) {
        // Already here: nudge QML to play it again.
        emit liveChanged();
        return;
    }
    request(m_index, QStringLiteral("live"));
}

void Viewer::libraryReloaded(const std::vector<int> &moved) {
    if (m_index < 0)
        return;
    const int now = m_index < int(moved.size()) ? moved[size_t(m_index)] : -1;
    if (now >= 0) {
        // The same photo: nothing on screen changes, only its position.
        m_index = now;
        return;
    }
    m_index = -1;
    close();
}
