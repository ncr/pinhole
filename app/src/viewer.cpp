#include "viewer.h"

#include "library.h"
#include "photogrid.h"
#include "serviceclient.h"

#include <QDateTime>
#include <QFile>
#include <QLocale>
#include <QNativeGestureEvent>
#include <QWheelEvent>

#include <cmath>

Viewer::Viewer(const Library *library, ServiceClient *service, QObject *parent)
    : QObject(parent), m_library(library), m_service(service) {
    for (auto changed : {&Viewer::openChanged, &Viewer::indexChanged, &Viewer::imageChanged, &Viewer::videoChanged, &Viewer::messageChanged,
                          &Viewer::liveChanged, &Viewer::effectChanged})
        connect(this, changed, this, &Viewer::loadingChanged);
    // No service after all: the motion asked for will not come.
    connect(service, &ServiceClient::announced, this, [this](const QString &name, const QJsonObject &details) {
        if (name == QLatin1String("health") && details.value("state").toString() == QLatin1String("no_service"))
            liveFailedWith(QStringLiteral("no_service"));
    });
}

bool Viewer::loading() const {
    if (!m_open || !m_message.isEmpty())
        return false;
    if (isVideo())
        return m_video.isEmpty();
    // An effect being fetched or made (a bounce or a long exposure from
    // the motion takes a few seconds).
    const bool effectComing = looping() ? m_liveKind != m_effect
                            : m_effect == QLatin1String("long_exposure") && m_longExposure.isEmpty();
    return m_stage != QLatin1String("original") || effectComing;
}

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
QString Viewer::savedEffect() const {
    if (!isLive())
        return QStringLiteral("live");
    switch (m_library->at(m_index).effect) {
    case LiveEffect::Loop: return QStringLiteral("loop");
    case LiveEffect::Bounce: return QStringLiteral("bounce");
    case LiveEffect::LongExposure: return QStringLiteral("long_exposure");
    case LiveEffect::None: break;
    }
    return QStringLiteral("live");
}

bool Viewer::looping() const {
    return m_effect == QLatin1String("loop") || m_effect == QLatin1String("bounce");
}

QString Viewer::motionKind() const {
    return looping() ? m_effect : QStringLiteral("live");
}

QString Viewer::stillKind() const {
    const QString longExposure = QStringLiteral("long_exposure");
    return savedEffect() == longExposure && m_effect != longExposure ? QStringLiteral("original") : QStringLiteral("full");
}

bool Viewer::isFavorite() const {
    return m_index >= 0 && m_index < m_library->count() && m_library->at(m_index).favorite;
}

QString Viewer::title() const {
    if (m_index < 0 || m_index >= m_library->count())
        return {};
    const QDateTime when = QDateTime::fromMSecsSinceEpoch(m_library->at(m_index).date);
    // "Saturday, 24 October 2026 · 16:16", in the locale's own words; the
    // window shows the time after the dot muted. Locales put a narrow
    // no-break space before AM/PM, which the monospace face draws with no
    // width at all ("1:12PM"): an ordinary space instead.
    const QLocale locale;
    QString time = locale.toString(when.time(), QLocale::ShortFormat);
    time.replace(QChar(0x202F), QLatin1Char(' ')).replace(QChar(0x00A0), QLatin1Char(' '));
    return locale.toString(when.date(), QLocale::LongFormat) + QStringLiteral(" · ") + time;
}

void Viewer::setMessage(const QString &message) {
    if (message == m_message)
        return;
    m_message = message;
    emit messageChanged();
}

void Viewer::liveFailedWith(const QString &code) {
    if (!m_liveLoading)
        return;
    m_liveLoading = false;
    emit liveChanged();
    const QString source = m_service->sourceName();
    QString message;
    if (code == QLatin1String("needs_sign_in"))
        message = tr("Not signed in to %1").arg(source);
    else if (code == QLatin1String("not_found"))
        message = tr("This Live Photo has no motion");
    else if (code == QLatin1String("no_service") || m_service->health() == QLatin1String("no_service"))
        message = tr("The Pinhole service is not running");
    // The service says it is offline before it answers so.
    else if (m_service->health() == QLatin1String("offline"))
        message = m_service->source().value("needs_sign_in").toBool()
                      ? tr("Offline, and the motion is not on this computer")
                      : tr("%1 is not there. Is its disk connected?").arg(source);
    else
        message = tr("Could not load the motion from %1").arg(source);
    emit liveFailed(message);
}

void Viewer::openAt(int index, QRectF from) {
    if (index < 0 || index >= m_library->count())
        return;
    m_origin = from;
    m_motion.reset();
    m_jumpFrom = -1;
    show(index);
    if (!m_open) {
        m_open = true;
        emit openChanged();
    }
}

bool Viewer::eventFilter(QObject *, QEvent *event) {
    if (!m_open)
        return false;
    if (event->type() == QEvent::NativeGesture) {
        auto *g = static_cast<QNativeGestureEvent *>(event);
        logInput(QStringLiteral("viewer gesture type=%1 value=%2 pos=%3,%4")
                     .arg(int(g->gestureType())).arg(g->value()).arg(g->position().x()).arg(g->position().y()));
        switch (g->gestureType()) {
        case Qt::BeginNativeGesture:
            m_pinchScale = 1;
            m_motion.pinchBegin(g->position().x(), g->position().y());
            break;
        case Qt::ZoomNativeGesture:
            // Each event's value is the change since the last; summed, not
            // multiplied (multiplying compounds, see the grid).
            m_pinchScale = std::max(0.05, m_pinchScale + g->value());
            m_motion.pinch(m_pinchScale, g->position().x(), g->position().y());
            break;
        case Qt::EndNativeGesture:
            if (m_motion.pinchEnd())
                emit closeRequested();
            break;
        default:
            break;
        }
        emit motionChanged();
        return true;
    }
    if (event->type() != QEvent::Wheel)
        return false;
    auto *wheel = static_cast<QWheelEvent *>(event);
    const QPoint px = wheel->pixelDelta();
    const QPoint angle = wheel->angleDelta();
    logInput(QStringLiteral("viewer wheel phase=%1 pixel=%2,%3 angle=%4,%5 inverted=%6 ts=%7")
                 .arg(int(wheel->phase())).arg(px.x()).arg(px.y()).arg(angle.x()).arg(angle.y())
                 .arg(wheel->inverted()).arg(wheel->timestamp()));
    if (wheel->modifiers() & Qt::ControlModifier) {
        // Ctrl+wheel zooms at the pointer, a notch a quarter.
        const double notches = (angle.y() != 0 ? angle.y() : angle.x()) / 120.0;
        if (notches != 0)
            m_motion.zoomBy(std::pow(1.25, notches), wheel->position().x(), wheel->position().y());
        emit motionChanged();
        return true;
    }
    if (wheel->phase() == Qt::NoScrollPhase && px.isNull()) {
        // A mouse wheel: a notch (120) is a photo, down for the next.
        m_notches += (angle.y() != 0 ? angle.y() : angle.x()) / 120.0;
        for (; m_notches <= -1; m_notches += 1)
            step(1);
        for (; m_notches >= 1; m_notches -= 1)
            step(-1);
        return true;
    }
    // A touchpad: the photos follow the fingers (viewermotion.h).
    const double t = double(wheel->timestamp());
    const QPointF d = px.isNull() ? QPointF(angle) / 8.0 : QPointF(px);
    switch (wheel->phase()) {
    case Qt::ScrollBegin:
        m_motion.begin(t);
        m_jumpFrom = -1; // caught mid-slide: the true neighbours again
        break;
    case Qt::ScrollUpdate:
    case Qt::ScrollMomentum:
    case Qt::NoScrollPhase:
        m_motion.move(t, d.x(), d.y());
        break;
    case Qt::ScrollEnd:
        if (const int delta = m_motion.end(t))
            step(delta);
        if (m_motion.takeClose())
            emit closeRequested();
        break;
    }
    emit motionChanged();
    return true;
}

int Viewer::count() const { return m_library->count(); }

QUrl Viewer::thumbnailAt(int index) const {
    if (index < 0 || index >= m_library->count())
        return {};
    return QUrl::fromLocalFile(QFile::decodeName(m_library->at(index).thumbPath));
}

QUrl Viewer::imageFor(int index) const {
    if (index < 0 || index >= m_library->count())
        return {};
    const auto found = m_mediums.find(m_library->at(index).id);
    return found != m_mediums.end() ? providerUrl(*found) : thumbnailAt(index);
}

qreal Viewer::aspectAt(int index) const {
    if (index < 0 || index >= m_library->count())
        return 1;
    const LibraryItem &item = m_library->at(index);
    return item.thumbH ? qreal(item.thumbW) / item.thumbH : 1;
}

void Viewer::remember(int index, const QString &mediumPath) {
    if (index < 0 || index >= m_library->count())
        return;
    const QByteArray id = m_library->at(index).id;
    if (m_mediums.value(id) == mediumPath)
        return;
    m_mediums.insert(id, mediumPath);
    m_revision += 1;
    emit imagesChanged();
}

void Viewer::fitPhoto() {
    const double a = aspect();
    const double w = std::min(m_viewport.width(), m_viewport.height() * a);
    m_motion.setPhoto(w, w / std::max(0.01, a), !isVideo());
    m_motion.setNeighbours(m_index > 0, m_index < m_library->count() - 1);
}

void Viewer::setViewport(qreal width, qreal height) {
    m_viewport = QSizeF(std::max<qreal>(1, width), std::max<qreal>(1, height));
    m_motion.setViewport(m_viewport.width(), m_viewport.height());
    fitPhoto();
    emit motionChanged();
}

void Viewer::advance(qreal seconds) {
    m_motion.advance(seconds);
    // A far jump's slide is over: back to the true neighbours.
    if (m_jumpFrom >= 0 && m_motion.offset() == 0)
        m_jumpFrom = -1;
    emit motionChanged();
}

void Viewer::zoomBy(qreal factor, qreal x, qreal y) {
    m_motion.zoomBy(factor, x, y);
    emit motionChanged();
}

void Viewer::zoomStep(int steps) {
    m_motion.zoomBy(std::pow(1.6, steps), m_viewport.width() / 2, m_viewport.height() / 2);
    emit motionChanged();
}

void Viewer::resetZoom() {
    m_motion.resetZoom();
    emit motionChanged();
}

void Viewer::touch(double timeMs) {
    if (!m_open)
        return;
    m_motion.touch(timeMs);
    emit motionChanged();
}

void Viewer::stopCoasting() {
    m_motion.stopCoasting();
    emit motionChanged();
}

void Viewer::panBy(qreal dx, qreal dy) {
    m_motion.pan(dx, dy);
    emit motionChanged();
}

void Viewer::close() {
    if (!m_open)
        return;
    m_open = false;
    m_generation += 1; // anything still arriving is for a closed viewer
    m_liveLoading = false;
    m_motion.reset();
    m_jumpFrom = -1;
    m_mediums.clear();
    emit openChanged();
    emit motionChanged();
}

void Viewer::goTo(int index) {
    index = std::clamp(index, 0, m_library->count() - 1);
    const int delta = index - m_index;
    if (std::abs(delta) <= 1) {
        step(delta);
        return;
    }
    // Far: as if it were the next (or previous) photo, so only these two
    // slide; the carousel travels the whole way.
    const int side = delta > 0 ? 1 : -1;
    m_jumpFrom = m_index;
    m_jumpSide = side;
    m_motion.stepped(delta, side);
    show(index);
    emit motionChanged();
}

int Viewer::beside(int k) const {
    if (m_jumpFrom >= 0 && k != 0) {
        if (k == -m_jumpSide)
            return m_jumpFrom;
        return -1;
    }
    const int i = m_index + k;
    return i >= 0 && i < m_library->count() ? i : -1;
}

void Viewer::step(int delta) {
    const int next = std::clamp(m_index + delta, 0, m_library->count() - 1);
    if (next == m_index)
        return;
    // The new photo slides in from where it lay beside the old one.
    m_jumpFrom = -1;
    m_motion.stepped(next - m_index);
    show(next);
    emit motionChanged();
}

void Viewer::jumpTo(int index) {
    if (index < 0 || index >= m_library->count() || index == m_index)
        return;
    m_motion.reset();
    m_jumpFrom = -1;
    show(index);
    emit motionChanged();
}

void Viewer::show(int index) {
    m_generation += 1;
    m_index = index;
    m_image = QUrl();
    m_stage = QStringLiteral("thumbnail");
    m_video = QUrl();
    m_live = QUrl();
    m_liveLoading = false;
    m_liveKind.clear();
    m_longExposure = QUrl();
    m_effect = isLive() ? m_chosen.value(m_library->at(index).id, savedEffect()) : QStringLiteral("live");
    setMessage({});
    fitPhoto();
    emit indexChanged();
    emit imageChanged();
    emit videoChanged();
    emit liveChanged();
    emit effectChanged();

    if (isVideo()) {
        request(index, QStringLiteral("video"));
    } else {
        request(index, QStringLiteral("medium"));
        showEffect();
    }
    // The neighbours, two each side: they show beside it while swiping.
    for (int d : {1, -1, 2, -2}) {
        const int n = index + d;
        if (n < 0 || n >= m_library->count() || m_library->at(n).video || m_mediums.contains(m_library->at(n).id))
            continue;
        const QByteArray id = m_library->at(n).id;
        m_service->fetch(id, QStringLiteral("medium"), [this, id](bool ok, const QJsonObject &result, const QString &) {
            if (!ok || !m_open)
                return;
            // Found again by its id: the library may have been reloaded.
            for (int i : {m_index - 2, m_index - 1, m_index + 1, m_index + 2})
                if (i >= 0 && i < m_library->count() && m_library->at(i).id == id)
                    remember(i, result.value("path").toString());
        });
    }
}

void Viewer::request(int index, const QString &kind) {
    const int generation = m_generation;
    m_service->fetch(m_library->at(index).id, kind, [this, generation, index, kind](bool ok, const QJsonObject &result,
                                                                                    const QString &code) {
        if (generation != m_generation)
            return;
        if (!ok) {
            // No medium rendition (some old photos): go straight to the full
            // one. Anything else is reported, and what is shown stays.
            if (kind == QLatin1String("medium") && code == QLatin1String("not_found")) {
                request(index, stillKind());
                return;
            }
            const bool motion = kind == QLatin1String("live") || kind == QLatin1String("loop") || kind == QLatin1String("bounce");
            const bool effect = kind == QLatin1String("loop") || kind == QLatin1String("bounce") || kind == QLatin1String("long_exposure");
            if ((motion && kind != motionKind()) || (effect && kind != m_effect))
                return; // a motion or an effect no longer shown
            // Asked for with Live: the button says why not.
            if (motion && m_liveLoading) {
                liveFailedWith(code);
                return;
            }
            if (kind == QLatin1String("live"))
                return;
            if (code == QLatin1String("needs_sign_in"))
                setMessage(tr("Not signed in to %1").arg(m_service->sourceName()));
            else if (effect)
                setMessage(tr("Could not show this effect"));
            else
                setMessage(tr("Could not load from %1").arg(m_service->sourceName()));
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
        } else if (kind == QLatin1String("live") || kind == QLatin1String("loop") || kind == QLatin1String("bounce")) {
            // Another effect chosen meanwhile: not this motion.
            if (kind != motionKind())
                return;
            m_live = playable;
            m_liveLoading = false;
            m_liveKind = kind;
            emit liveChanged();
        } else if (kind == QLatin1String("long_exposure")) {
            m_longExposure = providerUrl(path);
            emit effectChanged();
        } else {
            // Shown as Live after all, or as the long exposure again.
            if ((kind == QLatin1String("full") || kind == QLatin1String("original")) && kind != stillKind())
                return;
            m_image = providerUrl(path);
            // The last, sharpest stage: the photo as edited at full size (the
            // unedited original only when there is no edit).
            m_stage = kind == QLatin1String("full") || kind == QLatin1String("original") ? QStringLiteral("original") : kind;
            if (kind == QLatin1String("medium"))
                remember(index, path);
            emit imageChanged();
            // The medium is on screen; the full size follows.
            if (kind == QLatin1String("medium"))
                request(index, stillKind());
        }
    }, kind == QLatin1String("video") || kind == QLatin1String("live") || kind == QLatin1String("loop") || kind == QLatin1String("bounce"));
}

void Viewer::playLive() {
    if (!isLive() || m_liveLoading)
        return;
    if (!m_live.isEmpty() && m_liveKind == motionKind()) {
        // Already here: nudge QML to play it again.
        emit liveChanged();
        return;
    }
    // Another motion than the one here (an effect chosen since): not it.
    m_live = QUrl();
    m_liveKind.clear();
    m_liveLoading = true;
    emit liveChanged();
    // Nobody to ask: said now, not never.
    if (m_service->health() == QLatin1String("no_service")) {
        liveFailedWith(QStringLiteral("no_service"));
        return;
    }
    request(m_index, motionKind());
}

void Viewer::showEffect() {
    if (!isLive())
        return;
    if (looping() && m_liveKind != m_effect)
        request(m_index, m_effect);
    else if (m_effect == QLatin1String("long_exposure") && m_longExposure.isEmpty())
        request(m_index, QStringLiteral("long_exposure"));
}

void Viewer::setEffect(const QString &effect) {
    static const QStringList effects{"live", "loop", "bounce", "long_exposure"};
    if (!isLive() || effect == m_effect || !effects.contains(effect))
        return;
    const QString still = stillKind();
    const QString motion = motionKind();
    m_effect = effect;
    // The motion asked for with Live is not the one this effect plays.
    if (m_liveLoading && motionKind() != motion) {
        m_liveLoading = false;
        emit liveChanged();
    }
    if (effect == savedEffect())
        m_chosen.remove(m_library->at(m_index).id);
    else
        m_chosen.insert(m_library->at(m_index).id, effect);
    if (m_message == tr("Could not show this effect"))
        setMessage({});
    emit effectChanged();
    // The photo under it changes only for a long exposure set on the
    // iPhone; until the sharp one is here, the medium asks for the right one.
    if (stillKind() != still && m_stage == QLatin1String("original"))
        request(m_index, stillKind());
    showEffect();
}

void Viewer::nextEffect() {
    static const QStringList effects{"live", "loop", "bounce", "long_exposure"};
    setEffect(effects[(effects.indexOf(m_effect) + 1) % effects.size()]);
}

void Viewer::libraryReloaded(const std::vector<int> &moved) {
    if (m_index < 0)
        return;
    const int now = m_index < int(moved.size()) ? moved[size_t(m_index)] : -1;
    if (now >= 0) {
        // The same photo: nothing on screen changes, only its position.
        m_index = now;
        m_jumpFrom = -1;
        return;
    }
    m_index = -1;
    close();
}
