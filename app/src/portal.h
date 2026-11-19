#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariantMap>

// Choosing a folder with the desktop's own picker, through
// xdg-desktop-portal's FileChooser over D-Bus, as Omarchy apps do rather
// than drawing a Qt dialog; Qt Quick's own where the portal fails (a
// desktop with no FileChooser), never nothing as though cancelled.
class PortalFolderPicker : public QObject {
    Q_OBJECT
public:
    explicit PortalFolderPicker(QObject *parent = nullptr);
    void pick(const QString &title);
    // Qt Quick's own picker, in the window, which pick() falls back to.
    void fallBack(const QString &title);

signals:
    // The folder, or empty if the picker was cancelled or failed.
    void picked(const QString &folder);

private slots:
    void response(uint code, const QVariantMap &results);
    void chosen();
    void dismissed();

private:
    QString m_title;
    QPointer<QObject> m_dialog;
};
