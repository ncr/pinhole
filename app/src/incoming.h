#pragma once

#include <QJsonObject>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>

class ServiceClient;

// Photos and videos dropped on the window, added to the library of the
// source shown: copied into a folder, uploaded to iCloud, by the service's
// `add` (docs/service.md), one at a time. What a drop would bring, how far
// adding has got, and what could not be added. The photos then show the
// usual way, as the service tells of the library's change.
class Incoming : public QObject {
    Q_OBJECT
    // Files are being added now.
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    // "Adding 3 files to iCloud…", then "Added 3 files to iCloud" a while.
    Q_PROPERTY(QString status READ status NOTIFY changed)
    // While busy, how much has gone, 0 to 1.
    Q_PROPERTY(double progress READ progress NOTIFY changed)
    // What could not be added, a line each ("notes.txt: not a photo or a
    // video iCloud takes"), until dismissed or the next drop.
    Q_PROPERTY(QStringList problems READ problems NOTIFY changed)
    // "1 file not added", over them, and how many that is (none when
    // they are only notes on files added).
    Q_PROPERTY(QString problemsTitle READ problemsTitle NOTIFY changed)
    Q_PROPERTY(int failed READ failed NOTIFY changed)
public:
    explicit Incoming(ServiceClient *service, QObject *parent = nullptr);

    // What a drop of these would take: `files` on this computer of a kind
    // the source takes, and `folders` (looked into only when dropped: a
    // drag hovering must not walk a whole disk). Neither: the drop is not
    // for this window.
    Q_INVOKABLE QVariantMap wanted(const QList<QUrl> &urls) const;
    // Add them, after anything still being added.
    Q_INVOKABLE void add(const QList<QUrl> &urls);
    Q_INVOKABLE void dismiss();

    bool busy() const { return m_busy; }
    QString status() const { return m_status; }
    double progress() const { return m_progress; }
    QStringList problems() const { return m_problems; }
    QString problemsTitle() const;
    int failed() const { return m_failed; }

    // What the source takes, by extension, as the service says.
    QStringList accepts() const;
    // A file's problem in words, from the service's code for it.
    QString reason(const QString &code, const QString &message) const;

signals:
    void changed();

private:
    void heard(const QString &name, const QJsonObject &details);
    void finished(const QJsonObject &result);
    QString place() const;

    ServiceClient *m_service;
    bool m_busy = false;
    QString m_status;
    double m_progress = 0;
    QStringList m_problems;
    int m_failed = 0;
    // Files dropped while others are being added, sent after them.
    QList<QStringList> m_queue;
    int m_files = 0;
    QTimer m_fade;
};
