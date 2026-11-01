#pragma once

#include <QColor>
#include <QFileSystemWatcher>
#include <QHash>
#include <QObject>
#include <QProcess>
#include <QTimer>

class Theme : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString accent READ accent NOTIFY changed)
    Q_PROPERTY(QString accentForeground READ accentForeground NOTIFY changed)
    Q_PROPERTY(int radius READ radius NOTIFY changed)
    // Every colour of the Omarchy theme's colors.toml, live: surfaces from
    // darkest to lightest, text from muted to brightest, and the rest.
    Q_PROPERTY(QColor background READ background NOTIFY changed)
    Q_PROPERTY(QColor darkBackground READ darkBackground NOTIFY changed)
    Q_PROPERTY(QColor darkerBackground READ darkerBackground NOTIFY changed)
    Q_PROPERTY(QColor lighterBackground READ lighterBackground NOTIFY changed)
    Q_PROPERTY(QColor foreground READ foreground NOTIFY changed)
    Q_PROPERTY(QColor darkForeground READ darkForeground NOTIFY changed)
    Q_PROPERTY(QColor brightForeground READ brightForeground NOTIFY changed)
    Q_PROPERTY(QColor selection READ selection NOTIFY changed)
    Q_PROPERTY(QColor muted READ muted NOTIFY changed)
    Q_PROPERTY(QColor red READ red NOTIFY changed)
    Q_PROPERTY(QColor yellow READ yellow NOTIFY changed)
    Q_PROPERTY(QColor cyan READ cyan NOTIFY changed)
    Q_PROPERTY(QColor magenta READ magenta NOTIFY changed)
    Q_PROPERTY(QColor green READ green NOTIFY changed)
    Q_PROPERTY(QColor blue READ blue NOTIFY changed)
    Q_PROPERTY(bool light READ light NOTIFY changed)
    // Omarchy's typeface and its type scale's base size (shell.toml).
    Q_PROPERTY(QString font READ font CONSTANT)
    Q_PROPERTY(int fontSize READ fontSize NOTIFY changed)

public:
    explicit Theme(const QString &currentDirectory = {}, QObject *parent = nullptr, const QString &hyprctl = {});
    ~Theme() override;

    QString accent() const { return m_accent; }
    QString accentForeground() const;
    int radius() const { return m_radius; }
    QColor background() const { return color("background"); }
    QColor darkBackground() const { return color("dark_background"); }
    QColor darkerBackground() const { return color("darker_background"); }
    QColor lighterBackground() const { return color("lighter_background"); }
    QColor foreground() const { return color("foreground"); }
    QColor darkForeground() const { return color("dark_foreground"); }
    QColor brightForeground() const { return color("bright_foreground"); }
    QColor selection() const { return color("selection"); }
    QColor muted() const { return color("muted"); }
    QColor red() const { return color("red"); }
    QColor yellow() const { return color("yellow"); }
    QColor cyan() const { return color("cyan"); }
    QColor magenta() const { return color("magenta"); }
    QColor green() const { return color("green"); }
    QColor blue() const { return color("blue"); }
    bool light() const { return m_light; }
    QString font() const { return fontFamily(); }
    // The same, for what draws text away from QML (a mark's glyph).
    static QString fontFamily() { return QStringLiteral("JetBrainsMono Nerd Font"); }
    int fontSize() const { return m_fontSize; }

    static QString readAccent(const QString &path);
    // `name = "#rrggbb"` lines of a colors.toml, and whether its mode is light.
    static QHash<QString, QColor> readColors(const QString &path, bool *light = nullptr);

signals:
    void changed();

private:
    void reload();
    QColor color(const QString &key) const;
    void refreshRounding();

    QString m_directory;
    // Omarchy's personal shell settings (its text size), over the theme's.
    QString m_userShell;
    QString m_accent = "#FFD60A";
    QHash<QString, QColor> m_colors;
    bool m_light = false;
    int m_fontSize = 12;
    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
    QString m_hyprctl;
    int m_radius = 0;
    QProcess m_roundingProcess;
    QTimer m_roundingPoll, m_roundingTimeout;
};
