#pragma once

#include <QJsonObject>
#include <QObject>
#include <QString>

// Which marks the grid draws, as turned in the side panel's "In the grid":
// each kind's switch by its id ("pinhole/video", "<plugin id>/<name>"),
// kept for this computer in window.conf's [grid] as markChoices, a JSON
// object of the switches turned. A kind never turned is as its
// defaultShown says.
// None there, and nothing is written until a switch is turned.
class MarkChoices : public QObject {
    Q_OBJECT
    // Counted up on every change: bind to it with shown().
    Q_PROPERTY(int revision READ revision NOTIFY changed)
public:
    // `path`: window.conf, empty for the one in the config folder.
    explicit MarkChoices(const QString &path = {}, QObject *parent = nullptr);
    static QString defaultPath();

    Q_INVOKABLE bool shown(const QString &id, bool byDefault) const;
    Q_INVOKABLE void setShown(const QString &id, bool on);
    int revision() const { return m_revision; }

signals:
    void changed();

private:
    void write();

    QString m_path;
    QJsonObject m_choices;
    int m_revision = 0;
};
