// Theme watching follows Omacut (MIT, David Heinemeier Hansson).
#include "theme.h"

#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStandardPaths>
#include <algorithm>
#include <cmath>

Theme::Theme(const QString &directory, QObject *parent, const QString &hyprctl)
    : QObject(parent),
      m_directory(directory.isEmpty() ? QDir::homePath() + "/.local/state/omarchy/current" : directory),
      m_userShell(directory.isEmpty() ? QDir::homePath() + "/.config/omarchy/shell.toml" : directory + "/config/shell.toml") {
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(80);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_debounce.start(); });
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { m_debounce.start(); });
    connect(&m_debounce, &QTimer::timeout, this, &Theme::reload);

    // Corners follow Hyprland's effective rounding, which themes and personal
    // overrides can both set, so ask hyprctl rather than reading its config.
    // Without Hyprland the corners stay sharp, like Omarchy's default.
    m_hyprctl = !hyprctl.isEmpty() ? hyprctl
              : directory.isEmpty() && !qEnvironmentVariableIsEmpty("HYPRLAND_INSTANCE_SIGNATURE")
                  ? QStandardPaths::findExecutable("hyprctl") : QString();
    m_roundingTimeout.setSingleShot(true);
    m_roundingTimeout.setInterval(500);
    connect(&m_roundingTimeout, &QTimer::timeout, &m_roundingProcess, &QProcess::kill);
    connect(&m_roundingProcess, &QProcess::errorOccurred, this, [this] { m_roundingTimeout.stop(); });
    connect(&m_roundingProcess, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        m_roundingTimeout.stop();
        const auto value = QJsonDocument::fromJson(m_roundingProcess.readAllStandardOutput()).object().value("int");
        if (code != 0 || status != QProcess::NormalExit || !value.isDouble() || value.toInt(-1) < 0)
            return;
        if (value.toInt() != m_radius) {
            m_radius = value.toInt();
            emit changed();
        }
    });
    m_roundingPoll.setInterval(1000);
    connect(&m_roundingPoll, &QTimer::timeout, this, &Theme::refreshRounding);
    if (!m_hyprctl.isEmpty())
        m_roundingPoll.start();
    reload();
}

Theme::~Theme() {
    if (m_roundingProcess.state() != QProcess::NotRunning) {
        m_roundingProcess.kill();
        m_roundingProcess.waitForFinished(1000);
    }
}

void Theme::refreshRounding() {
    if (m_hyprctl.isEmpty() || m_roundingProcess.state() != QProcess::NotRunning)
        return;
    m_roundingProcess.start(m_hyprctl, {"-j", "getoption", "decoration:rounding"});
    m_roundingTimeout.start();
}

QString Theme::readAccent(const QString &path) {
    QFile file(path);
    if (file.open(QIODevice::ReadOnly)) {
        const QRegularExpression expression(R"re(^\s*accent\s*=\s*["'](#[0-9a-fA-F]{6})["'])re",
                                            QRegularExpression::MultilineOption);
        const auto match = expression.match(QString::fromUtf8(file.readAll()));
        if (match.hasMatch())
            return match.captured(1);
    }
    return "#FFD60A";
}

QHash<QString, QColor> Theme::readColors(const QString &path, bool *light) {
    QHash<QString, QColor> colors;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return colors;
    const QString text = QString::fromUtf8(file.readAll());
    const QRegularExpression entry(R"re(^\s*([a-z_]+)\s*=\s*["'](#[0-9a-fA-F]{6})["'])re", QRegularExpression::MultilineOption);
    for (auto it = entry.globalMatch(text); it.hasNext();) {
        const auto match = it.next();
        colors.insert(match.captured(1), QColor(match.captured(2)));
    }
    if (light)
        *light = QRegularExpression(R"re(^\s*mode\s*=\s*["']light["'])re", QRegularExpression::MultilineOption)
                     .match(text)
                     .hasMatch();
    return colors;
}

// A colour the theme names, else Tokyo Night's (Omarchy's default), so the
// app looks at home before Omarchy is installed too. A theme without the
// in-between shades gets them from its background and foreground.
QColor Theme::color(const QString &key) const {
    if (const auto found = m_colors.find(key); found != m_colors.end())
        return *found;
    static const QHash<QString, QColor> fallback{
        {"background", QColor("#1a1b26")},        {"dark_background", QColor("#13141c")},
        {"darker_background", QColor("#0e0e14")}, {"lighter_background", QColor("#24283b")},
        {"foreground", QColor("#a9b1d6")},        {"dark_foreground", QColor("#565f89")},
        {"bright_foreground", QColor("#c0caf5")}, {"selection", QColor("#292e42")},
        {"muted", QColor("#414868")},             {"red", QColor("#f7768e")},
        {"yellow", QColor("#e0af68")},            {"cyan", QColor("#449dab")},
        {"magenta", QColor("#ad8ee6")},           {"green", QColor("#9ece6a")},
        {"blue", QColor("#7aa2f7")}};
    if (m_colors.contains("background") && m_colors.contains("foreground")) {
        const QColor bg = m_colors.value("background"), fg = m_colors.value("foreground");
        auto mix = [](QColor a, QColor b, double t) {
            return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t, a.greenF() + (b.greenF() - a.greenF()) * t,
                                    a.blueF() + (b.blueF() - a.blueF()) * t);
        };
        if (key == "dark_background" || key == "darker_background")
            return mix(bg, m_light ? QColor(Qt::white) : QColor(Qt::black), key == "dark_background" ? 0.25 : 0.45);
        if (key == "lighter_background" || key == "selection" || key == "muted")
            return mix(bg, fg, key == "lighter_background" ? 0.08 : key == "selection" ? 0.15 : 0.3);
        if (key == "dark_foreground")
            return mix(fg, bg, 0.45);
        if (key == "bright_foreground")
            return fg;
    }
    return fallback.value(key, QColor("#a9b1d6"));
}

// Black or white, whichever reads on the accent (WCAG relative luminance).
QString Theme::accentForeground() const {
    const QColor c(m_accent);
    auto linear = [](double v) { return v <= .04045 ? v / 12.92 : std::pow((v + .055) / 1.055, 2.4); };
    const double luminance = .2126 * linear(c.redF()) + .7152 * linear(c.greenF()) + .0722 * linear(c.blueF());
    return luminance > .179 ? "black" : "white";
}

void Theme::reload() {
    refreshRounding();
    const auto paths = m_watcher.files() + m_watcher.directories();
    if (!paths.isEmpty())
        m_watcher.removePaths(paths);

    // A theme switch replaces symlinks and files, so watch the parents too and
    // re-arm after every change. Watching the nearest existing ancestor means
    // installing Omarchy later works without restarting the app.
    QString ancestor = m_directory;
    while (!QFileInfo::exists(ancestor) && ancestor != "/")
        ancestor = QFileInfo(ancestor).absolutePath();
    QStringList candidates{ancestor, QFileInfo(ancestor).absolutePath(), m_directory,
                           m_directory + "/theme", m_directory + "/theme/colors.toml",
                           QFileInfo(m_userShell).absolutePath(), m_userShell};
    candidates.removeDuplicates();
    for (const auto &path : candidates)
        if (QFileInfo::exists(path))
            m_watcher.addPath(path);

    const auto accent = readAccent(m_directory + "/theme/colors.toml");
    bool light = false;
    const auto colors = readColors(m_directory + "/theme/colors.toml", &light);
    // The type scale's base size, Omarchy's text size: the one set in its
    // settings, else the theme's shell tokens.
    int fontSize = 12;
    for (const QString &path : {m_userShell, m_directory + "/theme/shell.toml"}) {
        QFile shell(path);
        if (!shell.open(QIODevice::ReadOnly))
            continue;
        const auto match = QRegularExpression(R"re(^\s*base-size\s*=\s*(\d+))re", QRegularExpression::MultilineOption)
                               .match(QString::fromUtf8(shell.readAll()));
        if (match.hasMatch()) {
            fontSize = std::clamp(match.captured(1).toInt(), 8, 32);
            break;
        }
    }
    if (accent != m_accent || colors != m_colors || light != m_light || fontSize != m_fontSize) {
        m_accent = accent;
        m_colors = colors;
        m_light = light;
        m_fontSize = fontSize;
        emit changed();
    }
}
