#pragma once

#include <QFileSystemWatcher>
#include <QHash>
#include <QJsonValue>
#include <QLocale>
#include <QPointer>
#include <QSet>
#include <QTimer>
#include <QTranslator>

class QQmlEngine;

// Follows the language picked under Omarchy's Setup > Language. Catalogs are
// flat JSON objects from English source text to translation, one per Omarchy
// language code (i18n/da.json, i18n/pt-BR.json), so QML keeps plain
// qsTr("Save") and C++ keeps tr("Save"). A counted string,
// qsTr("%n clips", "", count), maps to an object of plural forms keyed by
// CLDR category ({"one": "%n klip", "other": "%n klip"}).
//
// Each plugin has its own catalog (its i18n folder), and a string is
// looked up by where it is written: one in a plugin's files in that
// plugin's catalog first, then Pinhole's; one of Pinhole's own in Pinhole's
// alone. So two catalogs can give the same English different words, and no
// plugin changes what Pinhole, or another plugin, says.
//
// Qt says only the file's name (a qsTr()'s "context"): a plugin's main file
// is told apart by its URL (Plugins::at), a qsTr() run as JavaScript by the
// file the engine is in; a plain `text: qsTr("…")` in a file named as
// another plugin's (two Studio.qml) is looked up in each of theirs in turn.
class Translator : public QTranslator {
    Q_OBJECT
    Q_PROPERTY(QString language READ language NOTIFY changed)

public:
    explicit Translator(const QString &languageFile = {}, const QString &catalogDirectory = ":/i18n",
                        QObject *parent = nullptr);

    QString language() const { return m_language; }
    bool isEmpty() const override { return m_strings.isEmpty(); }
    QString translate(const char *context, const char *sourceText, const char *disambiguation = nullptr,
                      int n = -1) const override;

    // The plugins' catalogs: each plugin's id and its folder, whose i18n
    // folder has them and whose files' strings are its.
    void setPluginCatalogs(const QList<std::pair<QString, QString>> &plugins);
    // The engine the window's QML runs in: a qsTr() called in JavaScript
    // says which file it is in, which tells two files of one name apart.
    void setEngine(QQmlEngine *engine);
    // A plugin's own words (its manifest's name and description): from its
    // catalog, else as written.
    QString pluginText(const QString &id, const QString &source) const;
    // The English a text shown is a translation of, in Pinhole's catalog
    // and the plugins': "Eksportuj JPEG" is "Export JPEG". Empty in
    // English, or for words no catalog has. For the palette to find a
    // command by either name.
    Q_INVOKABLE QStringList sourcesOf(const QString &text) const;
    // Which plugin a qsTr() context is from ("" for Pinhole's own): a file's
    // name, or a plugin's main file's (Plugins gives its URL the id). For
    // tests, and translate().
    QStringList ownersOf(const QString &context, const QString &calling = {}) const;

    static QString readLanguage(const QString &path);
    static QString pluralCategory(const QString &language, long long count);
    static QLocale formatLocale(const QString &language);

signals:
    void changed();

private:
    using Strings = QHash<QString, QJsonValue>;
    struct PluginCatalog {
        QString id, dir;
        // The names of its QML and JavaScript files, "Finder", "Colour":
        // a qsTr()'s context.
        QSet<QString> files;
        Strings strings;
    };
    void reload();
    Strings read(const QString &directory) const;
    QString lookup(const Strings &strings, const QString &source, int n) const;

    QString m_languageFile;
    QString m_catalogDirectory;
    QList<std::pair<QString, QString>> m_pluginDirs;
    QString m_language;
    Strings m_strings;
    // Pinhole's own files' names, which are its strings' contexts.
    QSet<QString> m_files;
    QList<PluginCatalog> m_plugins;
    QPointer<QQmlEngine> m_engine;
    // sourcesOf()'s, made the first time it is asked, again after a reload.
    mutable QHash<QString, QStringList> m_sources;
    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
};
