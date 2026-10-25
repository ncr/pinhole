#pragma once

#include <QObject>
#include <QString>
#include <QVariantMap>

// Choosing a folder with the desktop's own picker, through
// xdg-desktop-portal's FileChooser over D-Bus, as Omarchy apps do rather
// than drawing a Qt dialog.
class PortalFolderPicker : public QObject {
    Q_OBJECT
public:
    explicit PortalFolderPicker(QObject *parent = nullptr);
    void pick(const QString &title);

signals:
    // The folder, or empty if the picker was cancelled or failed.
    void picked(const QString &folder);

private slots:
    void response(uint code, const QVariantMap &results);
};
