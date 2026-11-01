#include "markchoices.h"

#include <QJsonDocument>
#include <QSettings>
#include <QStandardPaths>

namespace {

constexpr const char *kChoices = "grid/markChoices";
constexpr const char *kOldMarks = "grid/marks";

} // namespace

QString MarkChoices::defaultPath() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/pinhole/window.conf";
}

MarkChoices::MarkChoices(const QString &path, QObject *parent)
    : QObject(parent), m_path(path.isEmpty() ? defaultPath() : path) {
    QSettings settings(m_path, QSettings::IniFormat);
    if (settings.contains(kChoices)) {
        m_choices = QJsonDocument::fromJson(settings.value(kChoices).toString().toUtf8()).object();
        return;
    }
    if (!settings.contains(kOldMarks))
        return;
    // The four marks' bits, as the side panel kept them before (13: all
    // but Live), by the ids they have now.
    const int marks = settings.value(kOldMarks).toInt();
    m_choices = {{"pinhole/video", (marks & 1) != 0},
                 {"pinhole.live/live", (marks & 2) != 0},
                 {"pinhole.favourites/favorite", (marks & 4) != 0},
                 {"pinhole/stack", (marks & 8) != 0}};
    write();
}

bool MarkChoices::shown(const QString &id, bool byDefault) const {
    const QJsonValue choice = m_choices.value(id);
    return choice.isBool() ? choice.toBool() : byDefault;
}

void MarkChoices::setShown(const QString &id, bool on) {
    const QJsonValue choice = m_choices.value(id);
    if (choice.isBool() && choice.toBool() == on)
        return;
    m_choices.insert(id, on);
    write();
    ++m_revision;
    emit changed();
}

void MarkChoices::write() {
    QSettings settings(m_path, QSettings::IniFormat);
    settings.setValue(kChoices, QString::fromUtf8(QJsonDocument(m_choices).toJson(QJsonDocument::Compact)));
    settings.sync();
}
