#pragma once

#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>

class QScreen;
struct wl_display;

// What the compositor says of each display, through the Wayland colour
// management protocol (wp_color_management_v1, docs/hdr.md), read only:
// Pinhole never changes how a display is set up.
//
// For each output, its image description: the primaries and curve the
// compositor composes it in, its luminances (the range and SDR white,
// "reference"), and the target it is shown on. From these, the headroom:
// how many stops brighter than SDR white the display can show
// (target max over reference, else max over reference), and whether its
// gamut is wider than sRGB.
struct OutputColour {
    QString name;          // the output's, as Hyprland names it: eDP-1
    bool described = false; // the compositor gave a description
    QString primaries;     // named ("srgb", "display_p3", "bt2020"…), or "" for given ones
    double xy[8] = {};     // red, green, blue and white x, y, when given
    QString targetPrimaries;
    double targetXy[8] = {};
    QString transfer;      // "gamma22", "srgb", "compound_power_2_4", "st2084_pq", "hlg", "ext_linear", "power 2.4"…
    double minLum = 0, maxLum = 0, referenceLum = 0; // nits
    double targetMin = 0, targetMax = 0;            // nits, 0 if not given
    QString failed;        // why there is no description, if there is none

    // Stops above SDR white, at least 0.
    double headroom() const;
    // The gamut shown (the target's, else the primaries') wider than sRGB.
    bool wide() const;
    // One line for `pinhole display`.
    QString describe() const;
};

// Every output's, from a connection to the compositor: what it does not
// say is left out. `why` says why there are none (no colour management).
// Its own event queue: safe on Qt's own connection, between its events.
QList<OutputColour> readOutputColours(wl_display *display, QString *why = nullptr);

// The display the window is on, for QML and plugins (pinhole.display):
// whether it can show HDR, by how many stops, and whether it is wide
// gamut. Read as the window starts and when screens come and go; the
// output the window is on (setScreen), else the first.
// PINHOLE_HEADROOM=<stops> says the headroom instead, for tests.
class ColourDisplay : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool hdr READ hdr NOTIFY changed)
    Q_PROPERTY(double headroom READ headroom NOTIFY changed)
    Q_PROPERTY(bool wide READ wide NOTIFY changed)
public:
    explicit ColourDisplay(QObject *parent = nullptr);

    bool hdr() const { return headroom() > 0.05; }
    double headroom() const;
    bool wide() const;
    // The output whose description counts.
    void setScreen(QScreen *screen);
    // Read them all again.
    void refresh();

signals:
    void changed();

private:
    const OutputColour *current() const;
    QList<OutputColour> m_outputs;
    QPointer<QScreen> m_screen;
};

// `pinhole display`: each output's description, as the compositor gives
// it, with no window. 0, or 1 when there is no Wayland or no colour
// management.
int runDisplayCommand(int argc, char *argv[]);
