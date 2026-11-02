#include "display.h"

#include <QGuiApplication>
#include <QScreen>
#include <QWindow>
#include <qpa/qplatformwindow_p.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <unistd.h>
#include <vector>
#include <wayland-client.h>

#include "color-management-v1-client-protocol.h"

namespace {

// The protocol's names for its primaries and curves (color-management-v1.xml).
QString primariesName(uint32_t p) {
    static const char *names[] = {nullptr, "srgb", "pal_m", "pal", "ntsc", "generic_film", "bt2020", "cie1931_xyz", "dci_p3", "display_p3", "adobe_rgb"};
    return p < std::size(names) && names[p] ? QString::fromLatin1(names[p]) : QStringLiteral("primaries %1").arg(p);
}

QString transferName(uint32_t t) {
    static const char *names[] = {nullptr, "bt1886", "gamma22", "gamma28", "st240", "ext_linear", "log_100", "log_316", "xvycc", "srgb", "ext_srgb", "st2084_pq", "st428", "hlg", "compound_power_2_4"};
    return t < std::size(names) && names[t] ? QString::fromLatin1(names[t]) : QStringLiteral("curve %1").arg(t);
}

// Named primaries' chromaticities: red, green, blue (x, y).
bool namedXy(const QString &name, double xy[6]) {
    struct Named { const char *name; double v[6]; };
    static const Named all[] = {
        {"srgb", {0.64, 0.33, 0.30, 0.60, 0.15, 0.06}},
        {"pal_m", {0.67, 0.33, 0.21, 0.71, 0.14, 0.08}},
        {"pal", {0.64, 0.33, 0.29, 0.60, 0.15, 0.06}},
        {"ntsc", {0.63, 0.34, 0.31, 0.595, 0.155, 0.07}},
        {"generic_film", {0.681, 0.319, 0.243, 0.692, 0.145, 0.049}},
        {"bt2020", {0.708, 0.292, 0.170, 0.797, 0.131, 0.046}},
        {"cie1931_xyz", {1, 0, 0, 1, 0, 0}},
        {"dci_p3", {0.680, 0.320, 0.265, 0.690, 0.150, 0.060}},
        {"display_p3", {0.680, 0.320, 0.265, 0.690, 0.150, 0.060}},
        {"adobe_rgb", {0.64, 0.33, 0.21, 0.71, 0.15, 0.06}},
    };
    for (const Named &n : all)
        if (name == QLatin1String(n.name)) {
            std::copy(n.v, n.v + 6, xy);
            return true;
        }
    return false;
}

double area(const double xy[6]) {
    return std::abs((xy[2] - xy[0]) * (xy[5] - xy[1]) - (xy[4] - xy[0]) * (xy[3] - xy[1])) / 2;
}

// One read: the manager, the outputs and their descriptions as they come.
struct Read {
    wp_color_manager_v1 *manager = nullptr;
    uint32_t managerVersion = 0;
    struct Output {
        wl_output *output = nullptr;
        uint32_t global = 0;
        DescriptionReader reader;
        wp_color_management_output_v1 *cm = nullptr;
        wp_image_description_v1 *description = nullptr;
        wp_image_description_info_v1 *info = nullptr;
        bool ready = false;
    };
    std::vector<std::unique_ptr<Output>> outputs;
};

void onGlobal(void *data, wl_registry *registry, uint32_t name, const char *interface, uint32_t version) {
    auto *read = static_cast<Read *>(data);
    if (std::strcmp(interface, wp_color_manager_v1_interface.name) == 0 && !read->manager) {
        // The version it was generated for at most (docs/hdr.md, risk 9).
        read->managerVersion = std::min<uint32_t>(version, uint32_t(wp_color_manager_v1_interface.version));
        read->manager = static_cast<wp_color_manager_v1 *>(
            wl_registry_bind(registry, name, &wp_color_manager_v1_interface, read->managerVersion));
    } else if (std::strcmp(interface, wl_output_interface.name) == 0) {
        auto output = std::make_unique<Read::Output>();
        output->global = name;
        output->output = static_cast<wl_output *>(wl_registry_bind(registry, name, &wl_output_interface, std::min<uint32_t>(version, 4)));
        read->outputs.push_back(std::move(output));
    }
}

void onGlobalRemove(void *, wl_registry *, uint32_t) {}

const wl_registry_listener kRegistry = {onGlobal, onGlobalRemove};

// wl_output: only its name matters here.
const wl_output_listener kOutput = {
    [](void *, wl_output *, int32_t, int32_t, int32_t, int32_t, int32_t, const char *, const char *, int32_t) {},
    [](void *, wl_output *, uint32_t, int32_t, int32_t, int32_t) {},
    [](void *, wl_output *) {},
    [](void *, wl_output *, int32_t) {},
    [](void *data, wl_output *, const char *name) { static_cast<Read::Output *>(data)->reader.colour.name = QString::fromUtf8(name); },
    [](void *, wl_output *, const char *) {},
};

const wp_color_manager_v1_listener kManager = {
    [](void *, wp_color_manager_v1 *, uint32_t) {},
    [](void *, wp_color_manager_v1 *, uint32_t) {},
    [](void *, wp_color_manager_v1 *, uint32_t) {},
    [](void *, wp_color_manager_v1 *, uint32_t) {},
    [](void *, wp_color_manager_v1 *) {},
};

const wp_color_management_output_v1_listener kCmOutput = {
    [](void *, wp_color_management_output_v1 *) {},
};

const wp_image_description_v1_listener kDescription = {
    // failed
    [](void *data, wp_image_description_v1 *, uint32_t, const char *msg) {
        static_cast<Read::Output *>(data)->reader.colour.failed = QString::fromUtf8(msg);
    },
    // ready
    [](void *data, wp_image_description_v1 *, uint32_t) { static_cast<Read::Output *>(data)->ready = true; },
    // ready2
    [](void *data, wp_image_description_v1 *, uint32_t, uint32_t) { static_cast<Read::Output *>(data)->ready = true; },
};

OutputColour &colourOf(void *data) { return static_cast<DescriptionReader *>(data)->colour; }

const wp_image_description_info_v1_listener kInfo = {
    // done
    [](void *data, wp_image_description_info_v1 *) {
        auto *reader = static_cast<DescriptionReader *>(data);
        reader->done = true;
        reader->colour.described = true;
    },
    // icc_file: not read, closed.
    [](void *, wp_image_description_info_v1 *, int32_t fd, uint32_t) { close(fd); },
    // primaries
    [](void *data, wp_image_description_info_v1 *, int32_t rx, int32_t ry, int32_t gx, int32_t gy, int32_t bx, int32_t by,
       int32_t wx, int32_t wy) {
        const int32_t v[8] = {rx, ry, gx, gy, bx, by, wx, wy};
        for (int i = 0; i < 8; ++i)
            colourOf(data).xy[i] = v[i] / 1e6;
    },
    // primaries_named
    [](void *data, wp_image_description_info_v1 *, uint32_t p) { colourOf(data).primaries = primariesName(p); },
    // tf_power
    [](void *data, wp_image_description_info_v1 *, uint32_t eexp) {
        colourOf(data).transfer = QStringLiteral("power %1").arg(eexp / 10000.0);
    },
    // tf_named
    [](void *data, wp_image_description_info_v1 *, uint32_t tf) { colourOf(data).transfer = transferName(tf); },
    // luminances
    [](void *data, wp_image_description_info_v1 *, uint32_t min, uint32_t max, uint32_t reference) {
        OutputColour &c = colourOf(data);
        c.minLum = min / 1e4;
        c.maxLum = max;
        c.referenceLum = reference;
    },
    // target_primaries
    [](void *data, wp_image_description_info_v1 *, int32_t rx, int32_t ry, int32_t gx, int32_t gy, int32_t bx, int32_t by,
       int32_t wx, int32_t wy) {
        const int32_t v[8] = {rx, ry, gx, gy, bx, by, wx, wy};
        OutputColour &c = colourOf(data);
        for (int i = 0; i < 8; ++i)
            c.targetXy[i] = v[i] / 1e6;
        c.targetPrimaries = QStringLiteral("given");
    },
    // target_luminance
    [](void *data, wp_image_description_info_v1 *, uint32_t min, uint32_t max) {
        colourOf(data).targetMin = min / 1e4;
        colourOf(data).targetMax = max;
    },
    // target_max_cll, target_max_fall
    [](void *, wp_image_description_info_v1 *, uint32_t) {},
    [](void *, wp_image_description_info_v1 *, uint32_t) {},
};

// The window's preferred description being read: its information's events
// as an output's, and once done, said to the display.
struct Preferred : DescriptionReader {
    ColourDisplay::Feedback *feedback = nullptr;
};

void preferredDone(void *data, wp_image_description_info_v1 *);

const wp_image_description_info_v1_listener kPreferredInfo = [] {
    wp_image_description_info_v1_listener listener = kInfo;
    listener.done = preferredDone;
    return listener;
}();

} // namespace

const void *descriptionInfoListener() {
    return &kInfo;
}

// What the compositor would have the window's surface be
// (wp_color_management_surface_feedback_v1), on Qt's own event queue, so
// that its events come on the window's thread as they happen.
struct ColourDisplay::Feedback {
    ColourDisplay *owner = nullptr;
    wl_display *display = nullptr;
    wp_color_manager_v1 *manager = nullptr;
    bool parametric = false;
    bool bound = false;
    wl_surface *surface = nullptr;
    wp_color_management_surface_feedback_v1 *feedback = nullptr;
    // The one being read now; asked again once it is, if it changed
    // meanwhile.
    wp_image_description_v1 *description = nullptr;
    wp_image_description_info_v1 *info = nullptr;
    Preferred reading;
    bool again = false;

    ~Feedback() {
        detach();
        if (manager)
            wp_color_manager_v1_destroy(manager);
    }
    void bind();
    void attach(wl_surface *s);
    void detach();
    void ask();
    void forget();
};

namespace {

const wp_color_manager_v1_listener kFeedbackManager = {
    [](void *, wp_color_manager_v1 *, uint32_t) {},
    [](void *data, wp_color_manager_v1 *, uint32_t feature) {
        if (feature == WP_COLOR_MANAGER_V1_FEATURE_PARAMETRIC)
            static_cast<ColourDisplay::Feedback *>(data)->parametric = true;
    },
    [](void *, wp_color_manager_v1 *, uint32_t) {},
    [](void *, wp_color_manager_v1 *, uint32_t) {},
    [](void *, wp_color_manager_v1 *) {},
};

void onFeedbackGlobal(void *data, wl_registry *registry, uint32_t name, const char *interface, uint32_t version) {
    auto *f = static_cast<ColourDisplay::Feedback *>(data);
    if (std::strcmp(interface, wp_color_manager_v1_interface.name) != 0 || f->manager)
        return;
    f->manager = static_cast<wp_color_manager_v1 *>(wl_registry_bind(
        registry, name, &wp_color_manager_v1_interface, std::min<uint32_t>(version, uint32_t(wp_color_manager_v1_interface.version))));
    wp_color_manager_v1_add_listener(f->manager, &kFeedbackManager, f);
}

const wl_registry_listener kFeedbackRegistry = {onFeedbackGlobal, onGlobalRemove};

const wp_color_management_surface_feedback_v1_listener kSurfaceFeedback = {
    // preferred_changed
    [](void *data, wp_color_management_surface_feedback_v1 *, uint32_t) { static_cast<ColourDisplay::Feedback *>(data)->ask(); },
    // preferred_changed2
    [](void *data, wp_color_management_surface_feedback_v1 *, uint32_t, uint32_t) {
        static_cast<ColourDisplay::Feedback *>(data)->ask();
    },
};

void preferredReady(void *data) {
    auto *f = static_cast<ColourDisplay::Feedback *>(data);
    f->info = wp_image_description_v1_get_information(f->description);
    wp_image_description_info_v1_add_listener(f->info, &kPreferredInfo, static_cast<DescriptionReader *>(&f->reading));
}

const wp_image_description_v1_listener kPreferredDescription = {
    // failed: none for now
    [](void *data, wp_image_description_v1 *, uint32_t, const char *) { static_cast<ColourDisplay::Feedback *>(data)->forget(); },
    // ready, ready2
    [](void *data, wp_image_description_v1 *, uint32_t) { preferredReady(data); },
    [](void *data, wp_image_description_v1 *, uint32_t, uint32_t) { preferredReady(data); },
};

void preferredDone(void *data, wp_image_description_info_v1 *) {
    auto *reading = static_cast<Preferred *>(static_cast<DescriptionReader *>(data));
    reading->done = true;
    reading->colour.described = true;
    ColourDisplay::Feedback *f = reading->feedback;
    const OutputColour colour = reading->colour;
    // The information is gone with its done; the description is let go.
    wp_image_description_info_v1_destroy(f->info);
    f->info = nullptr;
    f->forget();
    f->owner->preferredRead(colour);
    if (f->again)
        f->ask();
}

} // namespace

void ColourDisplay::Feedback::bind() {
    bound = true;
    // Bound on a queue of its own, then handed to Qt's: its events come
    // with the window's from then on.
    wl_event_queue *queue = wl_display_create_queue(display);
    auto *wrapper = static_cast<wl_display *>(wl_proxy_create_wrapper(display));
    wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(wrapper), queue);
    wl_registry *registry = wl_display_get_registry(wrapper);
    wl_registry_add_listener(registry, &kFeedbackRegistry, this);
    wl_display_roundtrip_queue(display, queue);
    // What it supports.
    if (manager) {
        wl_display_roundtrip_queue(display, queue);
        wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(manager), nullptr);
    }
    wl_registry_destroy(registry);
    wl_proxy_wrapper_destroy(wrapper);
    wl_event_queue_destroy(queue);
}

void ColourDisplay::Feedback::attach(wl_surface *s) {
    detach();
    if (!s || !display)
        return;
    if (!bound)
        bind();
    if (!manager || !parametric)
        return;
    surface = s;
    feedback = wp_color_manager_v1_get_surface_feedback(manager, surface);
    wp_color_management_surface_feedback_v1_add_listener(feedback, &kSurfaceFeedback, this);
    ask();
}

void ColourDisplay::Feedback::forget() {
    if (info)
        wp_image_description_info_v1_destroy(info);
    if (description)
        wp_image_description_v1_destroy(description);
    info = nullptr;
    description = nullptr;
}

void ColourDisplay::Feedback::detach() {
    forget();
    if (feedback)
        wp_color_management_surface_feedback_v1_destroy(feedback);
    feedback = nullptr;
    surface = nullptr;
    again = false;
}

void ColourDisplay::Feedback::ask() {
    if (!feedback)
        return;
    if (description) {
        again = true;
        return;
    }
    again = false;
    reading = Preferred();
    reading.feedback = this;
    description = wp_color_management_surface_feedback_v1_get_preferred_parametric(feedback);
    wp_image_description_v1_add_listener(description, &kPreferredDescription, this);
    wl_display_flush(display);
}

double OutputColour::headroom() const {
    if (referenceLum <= 0)
        return 0;
    const double top = targetMax > 0 ? targetMax : maxLum;
    return top > referenceLum ? std::log2(top / referenceLum) : 0;
}

bool OutputColour::wide() const {
    double gamut[6];
    if (!targetPrimaries.isEmpty())
        std::copy(targetXy, targetXy + 6, gamut);
    else if (primaries.isEmpty())
        std::copy(xy, xy + 6, gamut);
    else if (!namedXy(primaries, gamut))
        return false;
    double srgb[6];
    namedXy(QStringLiteral("srgb"), srgb);
    return area(gamut) > area(srgb) * 1.02;
}

QString OutputColour::describe() const {
    const QString who = name.isEmpty() ? QStringLiteral("an output") : name;
    if (!described)
        return QStringLiteral("%1: no description%2").arg(who, failed.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(failed));
    // Given primaries by their name when they are one's.
    auto named = [](const QString &name, const double given[8]) {
        if (!name.isEmpty() && name != QLatin1String("given"))
            return name;
        for (const char *n : {"srgb", "display_p3", "bt2020", "adobe_rgb", "ntsc", "pal", "pal_m", "generic_film"}) {
            double v[6];
            namedXy(QLatin1String(n), v);
            bool same = true;
            for (int i = 0; i < 6; ++i)
                same = same && std::abs(v[i] - given[i]) < 0.002;
            if (same)
                return QString::fromLatin1(n);
        }
        return QStringLiteral("R %1,%2 G %3,%4 B %5,%6")
            .arg(given[0]).arg(given[1]).arg(given[2]).arg(given[3]).arg(given[4]).arg(given[5]);
    };
    const QString prim = named(primaries, xy);
    QString line = QStringLiteral("%1: %2 primaries, %3 curve; %4 to %5 nits, SDR white at %6")
                       .arg(who, prim, transfer.isEmpty() ? QStringLiteral("no") : transfer)
                       .arg(minLum)
                       .arg(maxLum)
                       .arg(referenceLum);
    if (targetMax > 0 && (targetMin != minLum || targetMax != maxLum))
        line += QStringLiteral("; shown at %1 to %2 nits").arg(targetMin).arg(targetMax);
    if (!targetPrimaries.isEmpty() && named(targetPrimaries, targetXy) != prim)
        line += QStringLiteral("; shown in %1").arg(named(targetPrimaries, targetXy));
    const double h = headroom();
    line += h > 0.05 ? QStringLiteral("; HDR, %1 stops above SDR white").arg(h, 0, 'f', 2) : QStringLiteral("; no HDR headroom");
    line += wide() ? QStringLiteral("; wider than sRGB") : QStringLiteral("; sRGB's gamut");
    return line;
}

QList<OutputColour> readOutputColours(wl_display *display, QString *why) {
    QList<OutputColour> result;
    if (!display) {
        if (why)
            *why = QStringLiteral("not on Wayland");
        return result;
    }
    wl_event_queue *queue = wl_display_create_queue(display);
    auto *wrapper = static_cast<wl_display *>(wl_proxy_create_wrapper(display));
    wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(wrapper), queue);
    wl_registry *registry = wl_display_get_registry(wrapper);
    Read read;
    wl_registry_add_listener(registry, &kRegistry, &read);
    wl_display_roundtrip_queue(display, queue);
    for (auto &o : read.outputs)
        wl_output_add_listener(o->output, &kOutput, o.get());
    if (read.manager)
        wp_color_manager_v1_add_listener(read.manager, &kManager, &read);
    // The outputs' names.
    wl_display_roundtrip_queue(display, queue);
    if (!read.manager) {
        if (why)
            *why = QStringLiteral("the compositor has no colour management (wp_color_manager_v1)");
    } else {
        for (auto &o : read.outputs) {
            o->cm = wp_color_manager_v1_get_output(read.manager, o->output);
            wp_color_management_output_v1_add_listener(o->cm, &kCmOutput, o.get());
            o->description = wp_color_management_output_v1_get_image_description(o->cm);
            wp_image_description_v1_add_listener(o->description, &kDescription, o.get());
        }
        wl_display_roundtrip_queue(display, queue);
        for (auto &o : read.outputs) {
            if (!o->ready)
                continue;
            o->info = wp_image_description_v1_get_information(o->description);
            wp_image_description_info_v1_add_listener(o->info, &kInfo, &o->reader);
        }
        // Each until its done.
        for (int i = 0; i < 4; ++i) {
            bool all = true;
            for (auto &o : read.outputs)
                all = all && (!o->info || o->reader.done);
            if (all)
                break;
            wl_display_roundtrip_queue(display, queue);
        }
    }
    for (auto &o : read.outputs) {
        result << o->reader.colour;
        // The info is gone with its done (a destructor event).
        if (o->info)
            wp_image_description_info_v1_destroy(o->info);
        if (o->description)
            wp_image_description_v1_destroy(o->description);
        if (o->cm)
            wp_color_management_output_v1_destroy(o->cm);
        if (wl_proxy_get_version(reinterpret_cast<wl_proxy *>(o->output)) >= WL_OUTPUT_RELEASE_SINCE_VERSION)
            wl_output_release(o->output);
        else
            wl_output_destroy(o->output);
    }
    if (read.manager)
        wp_color_manager_v1_destroy(read.manager);
    wl_registry_destroy(registry);
    wl_proxy_wrapper_destroy(wrapper);
    wl_display_flush(display);
    wl_event_queue_destroy(queue);
    return result;
}

ColourDisplay::ColourDisplay(QObject *parent) : QObject(parent) {
    refresh();
    if (qGuiApp) {
        connect(qGuiApp, &QGuiApplication::screenAdded, this, &ColourDisplay::refresh);
        connect(qGuiApp, &QGuiApplication::screenRemoved, this, &ColourDisplay::refresh);
    }
}

void ColourDisplay::refresh() {
    wl_display *display = nullptr;
    if (qGuiApp)
        if (auto *wayland = qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>())
            display = wayland->display();
    m_outputs = readOutputColours(display);
    emit changed();
}

ColourDisplay::~ColourDisplay() = default;

void ColourDisplay::followSurface(QWindow *window) {
    auto *wayland = qGuiApp ? qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>() : nullptr;
    auto *native = window ? window->nativeInterface<QNativeInterface::Private::QWaylandWindow>() : nullptr;
    if (!wayland || !native)
        return;
    m_feedback = std::make_unique<Feedback>();
    m_feedback->owner = this;
    m_feedback->display = wayland->display();
    connect(native, &QNativeInterface::Private::QWaylandWindow::surfaceCreated, this,
            [this, native] { m_feedback->attach(native->surface()); });
    connect(native, &QNativeInterface::Private::QWaylandWindow::surfaceDestroyed, this, [this] { m_feedback->detach(); });
    m_feedback->attach(native->surface());
}

const OutputColour *ColourDisplay::preferred() const {
    return m_preferred.described ? &m_preferred : nullptr;
}

void ColourDisplay::preferredRead(const OutputColour &colour) {
    const double before = headroom();
    const bool wideBefore = wide();
    m_preferred = colour;
    ++m_preferredCount;
    // Said when it is not what was known before (the outputs as the
    // window started, or the last one): the display changed under it.
    if (std::abs(headroom() - before) > 0.01 || wide() != wideBefore)
        std::fprintf(stderr, "pinhole: the window's display now: %s\n", qPrintable(colour.describe()));
    emit changed();
}

void ColourDisplay::setScreen(QScreen *screen) {
    if (screen == m_screen)
        return;
    m_screen = screen;
    emit changed();
}

const OutputColour *ColourDisplay::current() const {
    if (m_screen)
        for (const OutputColour &o : m_outputs)
            if (o.name == m_screen->name())
                return &o;
    return m_outputs.isEmpty() ? nullptr : &m_outputs.first();
}

double ColourDisplay::headroom() const {
    bool given = false;
    const double forced = qEnvironmentVariable("PINHOLE_HEADROOM").toDouble(&given);
    if (given)
        return std::max(forced, 0.0);
    if (const OutputColour *p = preferred())
        return p->headroom();
    const OutputColour *o = current();
    return o && o->described ? o->headroom() : 0;
}

bool ColourDisplay::wide() const {
    if (const OutputColour *p = preferred())
        return p->wide();
    const OutputColour *o = current();
    return o && o->described && o->wide();
}

int runDisplayCommand(int argc, char *argv[]) {
    if (argc < 2 || std::strcmp(argv[1], "display") != 0)
        return -1;
    wl_display *display = wl_display_connect(nullptr);
    if (!display) {
        std::fprintf(stderr, "pinhole: not on Wayland: no display to ask\n");
        return 1;
    }
    QString why;
    const QList<OutputColour> outputs = readOutputColours(display, &why);
    wl_display_disconnect(display);
    if (!why.isEmpty()) {
        std::fprintf(stderr, "pinhole: %s\n", qPrintable(why));
        return 1;
    }
    for (const OutputColour &o : outputs)
        std::printf("%s\n", qPrintable(o.describe()));
    return 0;
}
