#include "viewer.h"

#include "gainmap.h"
#include "library.h"
#include "timeline.h"
#include "photogrid.h"
#include "photoprovider.h"
#include "serviceclient.h"

#include <QDateTime>
#include <QFile>
#include <QFutureWatcher>
#include <QKeyEvent>
#include <QLocale>
#include <QNativeGestureEvent>
#include <QThreadPool>
#include <QWheelEvent>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <cmath>

namespace {
// Photos shown closer together than this (ms) are passing, not looked at:
// a held key repeats every 25.
constexpr int kPassing = 150;
}

Viewer::Viewer(const Library *library, ServiceClient *service, QObject *parent)
    : QObject(parent), m_library(library), m_service(service) {
    m_grabClock.start();
    m_fetchLater.setSingleShot(true);
    m_fetchLater.setInterval(kPassing);
    connect(&m_fetchLater, &QTimer::timeout, this, &Viewer::fetchShown);
    for (auto changed : {&Viewer::openChanged, &Viewer::indexChanged, &Viewer::imageChanged, &Viewer::videoChanged, &Viewer::messageChanged})
        connect(this, changed, this, &Viewer::loadingChanged);
    connect(this, &Viewer::indexChanged, this, &Viewer::favoriteChanged);
    // Another photo shown: its shape, and the adjustment drawn.
    connect(this, &Viewer::indexChanged, this, [this] {
        m_shownAdjust = m_index >= 0 && m_index < m_library->count() ? m_library->at(m_index).adjust : Adjustment();
        emit adjustChanged();
    });
}

bool Viewer::loading() const {
    if (!m_open || !m_message.isEmpty())
        return false;
    if (isVideo())
        return m_video.isEmpty();
    return m_stage != QLatin1String("original");
}

QUrl Viewer::providerUrl(const QString &path) {
    return QUrl(QStringLiteral("image://photo/") + QString::fromUtf8(QUrl::toPercentEncoding(path)));
}

QUrl Viewer::image() const {
    if (m_image.isEmpty() || m_index < 0 || m_index >= m_library->count())
        return m_image;
    return Adjustment::withAdjustment(m_image, m_library->at(m_index).adjust);
}

QUrl Viewer::thumbnail() const {
    return thumbnailAt(m_index);
}

qreal Viewer::aspect() const {
    if (m_index < 0 || m_index >= m_library->count())
        return 1;
    const LibraryItem &item = m_library->at(m_index);
    return item.thumbH ? qreal(item.thumbW) / item.thumbH : 1;
}

bool Viewer::isVideo() const { return m_index >= 0 && m_index < m_library->count() && m_library->at(m_index).video; }

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
    emit openAsked();
}

bool Viewer::eventFilter(QObject *, QEvent *event) {
    if (!m_open)
        return false;
    if (event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease) {
        auto *key = static_cast<QKeyEvent *>(event);
        const bool press = event->type() == QEvent::KeyPress;
        if (key->key() == Qt::Key_Shift) {
            m_motion.setHoldFast(press);
            return false;
        }
        const bool across = key->key() == Qt::Key_Left || key->key() == Qt::Key_Right;
        const bool down = key->key() == Qt::Key_Up || key->key() == Qt::Key_Down;
        if ((!across && !down) || !m_takesInput || (key->modifiers() & ~(Qt::ShiftModifier | Qt::KeypadModifier)))
            return false;
        // Zoomed in, the arrows look around the photo, as dragging it does
        // (a step a fifth of the window, with Shift half), held on and on.
        if (m_motion.zoomed()) {
            if (press) {
                const double step = (key->modifiers() & Qt::ShiftModifier ? 0.5 : 0.2) * m_motion.pageWidth();
                const double sign = key->key() == Qt::Key_Left || key->key() == Qt::Key_Up ? 1 : -1;
                panBy(across ? sign * step : 0, down ? sign * step : 0);
            }
            return true;
        }
        if (down)
            return false;
        // The system's repeats are not wanted: holding is timed here.
        if (key->isAutoRepeat())
            return true;
        if (press) {
            const int direction = key->key() == Qt::Key_Right ? 1 : -1;
            step(direction);
            m_heldKey = key->key();
            m_motion.hold(direction, key->modifiers() & Qt::ShiftModifier);
        } else if (key->key() == m_heldKey) {
            letGo();
        }
        emit motionChanged();
        return true;
    }
    // Keys let go while the window was elsewhere never arrive.
    if (event->type() == QEvent::FocusOut || event->type() == QEvent::WindowDeactivate)
        letGo();
    // The wheel and the touchpad go to what is over it.
    if (!m_takesInput)
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
    // A mouse wheel comes with no phases and with notches (on Wayland with
    // pixels too, 15 a notch); a touchpad has phases.
    if (wheel->phase() == Qt::NoScrollPhase && !angle.isNull()) {
        // Turned the natural (Apple) way, whatever the system's setting:
        // the opposite of a classic wheel, which reads positive pushed away
        // from you and rolled to the left.
        const QPoint classic = wheel->inverted() ? -angle : angle;
        if (std::abs(angle.y()) >= std::abs(angle.x())) {
            // The wheel zooms at the pointer, a notch a quarter: pulled
            // towards you, the photo comes closer.
            m_motion.zoomBy(std::pow(1.25, -classic.y() / 120.0), wheel->position().x(), wheel->position().y());
            emit motionChanged();
            return true;
        }
        // The thumb wheel: a notch (120) is a photo, the photos going the
        // way the wheel turns, as a swipe does.
        m_notches += classic.x() / 120.0;
        for (; m_notches >= 1; m_notches -= 1)
            step(1);
        for (; m_notches <= -1; m_notches += 1)
            step(-1);
        return true;
    }
    if (wheel->modifiers() & Qt::ControlModifier) {
        // Ctrl and a touchpad's scroll zoom at the pointer.
        const double notches = (angle.y() != 0 ? angle.y() : angle.x()) / 120.0;
        if (notches != 0)
            m_motion.zoomBy(std::pow(1.25, notches), wheel->position().x(), wheel->position().y());
        emit motionChanged();
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
            slide(delta);
        if (m_motion.takeClose())
            emit closeRequested();
        break;
    }
    emit motionChanged();
    return true;
}

void Viewer::setHidden(const QVariantList &hidden) {
    if (hidden == m_hiddenGiven)
        return;
    m_hiddenGiven = hidden;
    m_hiddenList.clear();
    m_hiddenList.reserve(size_t(hidden.size()));
    for (const QVariant &index : hidden)
        m_hiddenList.push_back(index.toInt());
    rebuildRow();
    emit hiddenChanged();
}

void Viewer::setTimeline(Timeline *timeline) {
    if (m_timeline)
        disconnect(m_timeline, nullptr, this, nullptr);
    m_timeline = timeline;
    if (timeline)
        connect(timeline, &Timeline::orderChanged, this, &Viewer::rebuildRow);
    rebuildRow();
}

void Viewer::rebuildRow() {
    const int n = m_library->count();
    std::vector<char> hidden;
    for (int i : m_hiddenList) {
        if (i < 0 || i >= n)
            continue;
        if (hidden.empty())
            hidden.assign(size_t(n), 0);
        hidden[size_t(i)] = 1;
    }
    m_rowMapped = !hidden.empty() || m_timeline || !m_among.empty();
    m_row.clear();
    if (!m_among.empty()) {
        m_rowPositions.assign(size_t(n), int(m_among.size()));
        for (int i : m_among) {
            m_rowPositions[size_t(i)] = int(m_row.size());
            m_row.push_back(i);
        }
    } else if (m_rowMapped) {
        m_row.reserve(size_t(n));
        m_rowPositions.resize(size_t(n));
        for (int place = 0; place < n; ++place) {
            const int i = m_timeline ? m_timeline->order()[size_t(place)] : place;
            m_rowPositions[size_t(i)] = int(m_row.size());
            if (hidden.empty() || !hidden[size_t(i)])
                m_row.push_back(i);
        }
    }
    m_jumpFrom = -1;
    if (m_open)
        fitPhoto();
    emit indexChanged();
    emit motionChanged();
}

int Viewer::count() const { return m_rowMapped ? int(m_row.size()) : m_library->count(); }

int Viewer::placeOf(int index) const {
    if (!m_rowMapped)
        return index;
    return index >= 0 && index < int(m_rowPositions.size()) ? m_rowPositions[size_t(index)] : count();
}

int Viewer::place() const { return m_index < 0 ? -1 : placeOf(m_index); }

int Viewer::placeFrom(int direction) const {
    const int p = placeOf(m_index);
    return direction > 0 && photoAt(p) != m_index ? p - 1 : p;
}

int Viewer::stepTarget(int delta, int *place) const {
    if (!delta || count() == 0)
        return -1;
    const int to = std::clamp(placeFrom(delta) + delta, 0, count() - 1);
    const int photo = photoAt(to);
    // Never back the other way: a hidden photo shown past either end of
    // the row has nothing beyond it.
    if (photo < 0 || photo == m_index || (rankOf(photo) > rankOf(m_index)) != (delta > 0))
        return -1;
    if (place)
        *place = to;
    return photo;
}

int Viewer::rankOf(int index) const {
    if (!m_among.empty())
        return placeOf(index);
    return m_timeline ? m_timeline->positionOf(index) : index;
}

int Viewer::photoAt(int place) const {
    if (place < 0 || place >= count())
        return -1;
    return m_rowMapped ? m_row[size_t(place)] : place;
}

QUrl Viewer::thumbnailAt(int index) const {
    if (index < 0 || index >= m_library->count())
        return {};
    // In sRGB, as the grid draws it (FileProvider), turned and cut as the
    // person did.
    return Adjustment::withAdjustment(FileProvider::urlOf(QFile::decodeName(m_library->at(index).thumbPath)),
                                      m_library->at(index).adjust);
}

QUrl Viewer::imageFor(int index) const {
    if (index < 0 || index >= m_library->count())
        return {};
    const auto found = m_mediums.find(m_library->at(index).id);
    return found != m_mediums.end() ? Adjustment::withAdjustment(providerUrl(*found), m_library->at(index).adjust)
                                    : thumbnailAt(index);
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
    m_motion.setNeighbours(rowBeside(-1) >= 0, rowBeside(1) >= 0);
}

void Viewer::setViewport(qreal width, qreal height) {
    m_viewport = QSizeF(std::max<qreal>(1, width), std::max<qreal>(1, height));
    m_motion.setViewport(m_viewport.width(), m_viewport.height());
    fitPhoto();
    emit motionChanged();
}

void Viewer::advance(qreal seconds) {
    m_motion.advance(seconds);
    // A held key brought the next photo; the carousel is already on its way.
    if (const int delta = m_motion.takeSteps()) {
        if (const int next = stepTarget(delta); next >= 0) {
            m_jumpFrom = -1;
            show(next);
        }
    }
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

void Viewer::grab() {
    m_motion.begin(double(m_grabClock.elapsed()), true);
    m_jumpFrom = -1; // caught mid-slide: the true neighbours again
    emit motionChanged();
}

void Viewer::drag(qreal dx, qreal dy) {
    m_motion.move(double(m_grabClock.elapsed()), dx, dy);
    emit motionChanged();
}

void Viewer::drop() {
    if (const int delta = m_motion.end(double(m_grabClock.elapsed())))
        slide(delta);
    if (m_motion.takeClose())
        emit closeRequested();
    emit motionChanged();
}

void Viewer::setTakesInput(bool takes) {
    if (takes == m_takesInput)
        return;
    m_takesInput = takes;
    if (!takes)
        letGo();
    emit takesInputChanged();
}

void Viewer::letGo() {
    if (!m_heldKey)
        return;
    m_heldKey = 0;
    m_motion.release();
    emit motionChanged();
}

void Viewer::openAmong(const std::vector<int> &photos, int index) {
    m_among.clear();
    std::vector<char> seen(size_t(m_library->count()), 0);
    for (int i : photos)
        if (i >= 0 && i < m_library->count() && !std::exchange(seen[size_t(i)], 1))
            m_among.push_back(i);
    if (index < 0 || index >= m_library->count() || !seen[size_t(index)])
        m_among.clear();
    rebuildRow();
    openAt(index, {});
}

void Viewer::close() {
    if (!m_open)
        return;
    letGo();
    m_open = false;
    if (!m_among.empty()) {
        m_among.clear();
        rebuildRow();
    }
    m_generation += 1; // anything still arriving is for a closed viewer
    m_motion.reset();
    m_jumpFrom = -1;
    m_mediums.clear();
    m_fetchLater.stop();
    m_lastShown.invalidate();
    emit openChanged();
    emit motionChanged();
}

void Viewer::goTo(int place) {
    place = std::clamp(place, 0, count() - 1);
    const int index = photoAt(place);
    if (index < 0 || index == m_index)
        return;
    const bool forward = rankOf(index) > rankOf(m_index);
    const int delta = place - placeFrom(forward ? 1 : -1);
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
    return rowBeside(k);
}

int Viewer::rowBeside(int k) const {
    if (k == 0)
        return m_index;
    // Always on its side: placeFrom is the place before a hidden photo
    // going forwards, the one after it going back.
    return photoAt(placeFrom(k) + k);
}

void Viewer::step(int delta) {
    int to = 0;
    const int next = stepTarget(delta, &to);
    if (next < 0)
        return;
    m_jumpFrom = -1;
    m_motion.jumped(to - placeFrom(delta));
    show(next);
    emit motionChanged();
}

void Viewer::slide(int delta) {
    int to = 0;
    const int next = stepTarget(delta, &to);
    if (next < 0)
        return;
    // The new photo slides in from where it lay beside the old one.
    m_jumpFrom = -1;
    m_motion.stepped(to - placeFrom(delta));
    show(next);
    emit motionChanged();
}

void Viewer::jumpTo(int place) {
    const int index = photoAt(place);
    if (index < 0 || index == m_index)
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
    setMessage({});
    setHdr({});
    fitPhoto();
    emit indexChanged();
    emit imageChanged();
    emit videoChanged();

    const bool passing = m_lastShown.isValid() && m_lastShown.elapsed() < kPassing;
    m_lastShown.start();
    if (passing)
        m_fetchLater.start();
    else
        fetchShown();
}

void Viewer::fetchShown() {
    m_fetchLater.stop();
    if (m_index < 0 || m_index >= m_library->count())
        return;
    const int index = m_index;
    request(index, isVideo() ? QStringLiteral("video") : QStringLiteral("medium"));
    // The neighbours in the row, two each side: they show beside it while
    // swiping (not the hidden photos between).
    for (int d : {1, -1, 2, -2}) {
        const int n = rowBeside(d);
        if (n < 0 || m_library->at(n).video || m_mediums.contains(m_library->at(n).id))
            continue;
        const QByteArray id = m_library->at(n).id;
        m_service->fetch(id, QStringLiteral("medium"), [this, id](bool ok, const QJsonObject &result, const QString &) {
            if (!ok || !m_open)
                return;
            // Found again by its id: the library may have been reloaded.
            for (int d : {-2, -1, 1, 2})
                if (const int i = rowBeside(d); i >= 0 && m_library->at(i).id == id)
                    remember(i, result.value("path").toString());
        });
    }
}

void Viewer::request(int index, const QString &kind) {
    const int generation = m_generation;
    const QByteArray id = m_library->at(index).id;
    m_service->fetch(id, kind, [this, generation, id, kind](bool ok, const QJsonObject &result, const QString &code) {
        if (generation != m_generation)
            return;
        // The photo shown still, wherever a reload since has put it: never
        // the position it was asked from, which may be another photo's now.
        if (m_index < 0 || m_index >= m_library->count() || m_library->at(m_index).id != id)
            return;
        const int index = m_index;
        if (!ok) {
            // No medium rendition (some old photos): go straight to the full
            // one. Anything else is reported, and what is shown stays.
            if (kind == QLatin1String("medium") && code == QLatin1String("not_found")) {
                request(index, QStringLiteral("full"));
                return;
            }
            if (code == QLatin1String("needs_sign_in"))
                setMessage(tr("Not signed in to %1").arg(m_service->sourceName()));
            // The file is there, but nothing can be made of it: asking again
            // gives the same.
            else if (code == QLatin1String("unreadable"))
                setMessage(tr("This file is damaged, so it cannot be shown"));
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
        } else {
            m_image = providerUrl(path);
            // The last, sharpest stage: the photo as edited at full size (the
            // unedited original only when there is no edit).
            m_stage = kind == QLatin1String("full") ? QStringLiteral("original") : kind;
            if (kind == QLatin1String("medium"))
                remember(index, path);
            emit imageChanged();
            readHdrOf(path);
            // The medium is on screen; the full size follows.
            if (kind == QLatin1String("medium"))
                request(index, QStringLiteral("full"));
        }
    }, kind == QLatin1String("video"));
}

void Viewer::readHdrOf(const QString &path) {
    const int generation = m_generation;
    auto *watcher = new QFutureWatcher<QVariantMap>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, generation] {
        watcher->deleteLater();
        // Still the photo it was read for; a full size read after its
        // medium has the last word.
        if (generation == m_generation)
            setHdr(watcher->result());
    });
    // On a thread of its own, at low priority: never one of the pool's,
    // which decode the very photo at the same moment.
    static QThreadPool *reading = [] {
        auto *pool = new QThreadPool;
        pool->setMaxThreadCount(1);
        pool->setThreadPriority(QThread::LowPriority);
        return pool;
    }();
    watcher->setFuture(QtConcurrent::run(reading, [path] { return readHdr(path).toMap(); }));
}

void Viewer::setHdr(const QVariantMap &hdr) {
    if (hdr == m_hdr)
        return;
    m_hdr = hdr;
    emit hdrChanged();
}

void Viewer::libraryReloaded(const std::vector<int> &moved) {
    auto mapped = [&](int old) { return old >= 0 && old < int(moved.size()) ? moved[size_t(old)] : -1; };
    // The photo shown turned or cut since: its pictures and its shape.
    const Adjustment before = m_index >= 0 && m_index < m_library->count() ? m_shownAdjust : Adjustment();
    for (int &i : m_hiddenList)
        i = mapped(i);
    std::erase(m_hiddenList, -1);
    for (int &i : m_among)
        i = mapped(i);
    std::erase(m_among, -1);
    // As given, too: what `hidden` reads, and what the same given again is
    // compared with.
    const QVariantList hiddenBefore = std::exchange(m_hiddenGiven, QVariantList(m_hiddenList.begin(), m_hiddenList.end()));
    if (m_index >= 0) {
        if (const int now = mapped(m_index); now >= 0) {
            // The same photo: nothing on screen changes, only its position,
            // and maybe whether it is a favourite.
            m_index = now;
            m_jumpFrom = -1;
            emit favoriteChanged();
        } else {
            m_index = -1;
            close();
        }
    }
    rebuildRow();
    if (m_hiddenGiven != hiddenBefore)
        emit hiddenChanged();
    const Adjustment now = m_index >= 0 && m_index < m_library->count() ? m_library->at(m_index).adjust : Adjustment();
    if (now != before) {
        m_shownAdjust = now;
        emit imageChanged();
        emit adjustChanged();
    }
    // Any photo's: what imageFor() and thumbnailAt() give follows.
    m_revision += 1;
    emit imagesChanged();
}
