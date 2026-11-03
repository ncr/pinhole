#pragma once

#include <QObject>
#include <QStringList>
#include <QVariantList>

// The same offline handbook in the window and `pinhole plugin docs`.
// Reading it is explicit: no Markdown parsing on the launch path.
class PluginGuide : public QObject {
    Q_OBJECT
    Q_PROPERTY(int api READ api CONSTANT)
public:
    using QObject::QObject;
    int api() const;
    static QStringList documents();
    static QString document(const QString &name);
    Q_INVOKABLE QVariantList sections(const QString &name) const;
    Q_INVOKABLE QString markdown(const QString &name, const QString &section = {}) const;
    Q_INVOKABLE QVariantMap destination(const QString &link, const QString &current) const;
    Q_INVOKABLE void copy(const QString &text) const;
};
