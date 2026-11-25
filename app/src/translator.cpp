// Translator follows Omarchy's apps (MIT, David Heinemeier Hansson).
#include "translator.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QQmlEngine>
#include <QRegularExpression>
#include <QThread>
#include <QUrl>
#include <private/qqmlcontextdata_p.h>
#include <private/qv4engine_p.h>

#include <algorithm>

namespace {

// The names of the QML and JavaScript files in a folder, as Qt makes a
// qsTr()'s context of them: "Finder" for Finder.qml.
QSet<QString> filesIn(const QString &directory, QDirIterator::IteratorFlags flags) {
    QSet<QString> files;
    QDirIterator it(directory, {"*.qml", "*.js"}, QDir::Files, flags);
    while (it.hasNext()) {
        const QFileInfo info(it.next());
        // A helper's build folder is not the plugin.
        if (!info.filePath().contains(QLatin1String("/target/")))
            files.insert(info.completeBaseName());
    }
    return files;
}

} // namespace

Translator::Translator(const QString &languageFile, const QString &catalogDirectory, QObject *parent)
    : QTranslator(parent),
      m_languageFile(languageFile.isEmpty() ? QDir::homePath() + "/.config/omarchy/language" : languageFile),
      m_catalogDirectory(catalogDirectory) {
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(80);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_debounce.start(); });
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { m_debounce.start(); });
    connect(&m_debounce, &QTimer::timeout, this, &Translator::reload);
    reload();
}

// One registry code on the first line; anything else, or no file, is English.
QString Translator::readLanguage(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return "en";
    const QString code = QString::fromUtf8(file.readLine()).trimmed();
    static const QRegularExpression valid("^[a-z]{2,3}(-[A-Za-z]{2})?$");
    return valid.match(code).hasMatch() ? code : "en";
}

// CLDR cardinal plural categories for whole counts, for Omarchy's languages
// (the same rules as Omarchy's shell/Commons/Plural.js).
QString Translator::pluralCategory(const QString &language, long long count) {
    const long long n = count < 0 ? -count : count, n10 = n % 10, n100 = n % 100;
    const bool millions = n != 0 && n % 1000000 == 0;
    const bool teens = n100 >= 12 && n100 <= 14;
    const QString code = language;
    auto in = [&](std::initializer_list<const char *> codes) {
        for (const char *c : codes) if (code == QLatin1String(c)) return true;
        return false;
    };
    if (in({"ja", "ko", "zh-CN", "zh-TW", "th", "vi", "id", "ms"})) return "other";
    if (in({"hi", "bn", "fa", "si"})) return n <= 1 ? "one" : "other";
    if (in({"fr", "pt-BR"})) return n <= 1 ? "one" : millions ? "many" : "other";
    if (in({"ca", "es-MX", "it", "pt"})) return n == 1 ? "one" : millions ? "many" : "other";
    if (code == "fil") return (n10 == 4 || n10 == 6 || n10 == 9) ? "other" : "one";
    if (code == "is") return n10 == 1 && n100 != 11 ? "one" : "other";
    if (in({"cs", "sk"})) return n == 1 ? "one" : n >= 2 && n <= 4 ? "few" : "other";
    if (code == "pl") return n == 1 ? "one" : n10 >= 2 && n10 <= 4 && !teens ? "few" : "many";
    if (in({"ru", "uk"})) return n10 == 1 && n100 != 11 ? "one" : n10 >= 2 && n10 <= 4 && !teens ? "few" : "many";
    if (in({"hr", "sr"})) return n10 == 1 && n100 != 11 ? "one" : n10 >= 2 && n10 <= 4 && !teens ? "few" : "other";
    if (code == "sl") return n100 == 1 ? "one" : n100 == 2 ? "two" : n100 >= 3 && n100 <= 4 ? "few" : "other";
    if (code == "lt") return n100 >= 11 && n100 <= 19 ? "other" : n10 == 1 ? "one" : n10 >= 2 ? "few" : "other";
    if (code == "lv") return n10 == 0 || (n100 >= 11 && n100 <= 19) ? "zero" : n10 == 1 ? "one" : "other";
    if (code == "ro") return n == 1 ? "one" : n == 0 || (n100 >= 1 && n100 <= 19) ? "few" : "other";
    if (code == "ar") {
        if (n <= 2) return n == 0 ? "zero" : n == 1 ? "one" : "two";
        return n100 >= 3 && n100 <= 10 ? "few" : n100 >= 11 ? "many" : "other";
    }
    if (code == "he") return n == 1 ? "one" : n == 2 ? "two" : "other";
    if (code == "ga") return n == 1 ? "one" : n == 2 ? "two" : n >= 3 && n <= 6 ? "few" : n >= 7 && n <= 10 ? "many" : "other";
    return n == 1 ? "one" : "other";
}

// The locale Omarchy formats this language with, from its registry, so the
// app's numbers and dates match the desktop's (Arabic there uses Western
// digits); without Omarchy, Qt's own locale for the code.
QLocale Translator::formatLocale(const QString &language) {
    QFile registry(qEnvironmentVariable("OMARCHY_PATH") + "/default/i18n/locales.json");
    if (registry.open(QIODevice::ReadOnly)) {
        const QString name = QJsonDocument::fromJson(registry.readAll()).object()
                                 .value(language).toObject().value("formatLocale").toString();
        if (!name.isEmpty())
            return QLocale(name);
    }
    return QLocale(language);
}

QString Translator::lookup(const Strings &strings, const QString &source, int n) const {
    // A null string tells Qt there is no translation, so it shows the English.
    const QJsonValue value = strings.value(source);
    if (value.isString())
        return value.toString();
    if (value.isObject()) {
        const QJsonObject forms = value.toObject();
        const QString form = forms.value(n < 0 ? "other" : pluralCategory(m_language, n)).toString();
        return form.isEmpty() ? forms.value("other").toString() : form;
    }
    return {};
}

QStringList Translator::ownersOf(const QString &context, const QString &calling) const {
    // A plugin's main file is loaded from a URL that names the plugin
    // (Plugins::at: Main.qml?plugin=<id>&load=<n>), and Qt makes its
    // bindings' context of that whole name, less its last four letters:
    // "Main.qml?plugin=pinhole.mosaic&lo".
    static const QRegularExpression named(QStringLiteral("\\?plugin=([^&]+)&"));
    if (const auto match = named.match(context); match.hasMatch())
        return {QUrl::fromPercentEncoding(match.captured(1).toUtf8()), QString()};
    // A qsTr() in JavaScript: the file it is in, when that is the one the
    // context was made of (and not code further out that made this one's
    // object). A main file's context is cut at the last dot, the id's.
    if (!calling.isEmpty()) {
        const QUrl url(calling);
        const QString file = url.fileName();
        if (context == QFileInfo(file).completeBaseName() || context.section('?', 0, 0) == file) {
            const QString path = url.isLocalFile() ? url.toLocalFile() : QString();
            for (const PluginCatalog &plugin : m_plugins)
                if (!path.isEmpty() && path.startsWith(plugin.dir + '/'))
                    return {plugin.id, QString()};
            if (url.scheme() == QLatin1String("qrc"))
                return {QString()};
        }
    }
    // Else by the file's name: the plugins with a file of that name, and
    // Pinhole, first if it has one too (and for a C++ class's tr()).
    QStringList owners;
    for (const PluginCatalog &plugin : m_plugins)
        if (plugin.files.contains(context))
            owners << plugin.id;
    if (owners.isEmpty() || m_files.contains(context))
        owners.prepend(QString());
    else
        owners.append(QString());
    return owners;
}

QString Translator::translate(const char *context, const char *sourceText, const char *, int n) const {
    const QString source = QString::fromUtf8(sourceText);
    if (m_plugins.isEmpty())
        return lookup(m_strings, source, n);
    // Called in JavaScript: in which file, from the engine, on its thread.
    QString calling;
    if (m_engine && QThread::currentThread() == m_engine->thread())
        if (const QQmlRefPointer<QQmlContextData> data = m_engine->handle()->callingQmlContext())
            calling = data->urlString();
    for (const QString &owner : ownersOf(QString::fromUtf8(context), calling)) {
        const Strings *strings = &m_strings;
        if (!owner.isEmpty()) {
            const auto plugin = std::find_if(m_plugins.cbegin(), m_plugins.cend(),
                                             [&](const PluginCatalog &p) { return p.id == owner; });
            if (plugin == m_plugins.cend())
                continue;
            strings = &plugin->strings;
        }
        if (const QString text = lookup(*strings, source, n); !text.isNull())
            return text;
    }
    return {};
}

void Translator::setEngine(QQmlEngine *engine) { m_engine = engine; }

QString Translator::pluginText(const QString &id, const QString &source) const {
    for (const PluginCatalog &plugin : m_plugins)
        if (plugin.id == id)
            if (const QString text = lookup(plugin.strings, source, -1); !text.isNull())
                return text;
    return source;
}

QStringList Translator::sourcesOf(const QString &text) const {
    if (m_language == QLatin1String("en"))
        return {};
    if (m_sources.isEmpty()) {
        const auto add = [this](const Strings &strings) {
            for (auto it = strings.cbegin(); it != strings.cend(); ++it)
                if (it.value().isString() && it.value().toString() != it.key()) {
                    QStringList &sources = m_sources[it.value().toString()];
                    if (!sources.contains(it.key()))
                        sources << it.key();
                }
        };
        add(m_strings);
        for (const PluginCatalog &plugin : m_plugins)
            add(plugin.strings);
    }
    return m_sources.value(text);
}

void Translator::setPluginCatalogs(const QList<std::pair<QString, QString>> &plugins) {
    if (plugins == m_pluginDirs)
        return;
    m_pluginDirs = plugins;
    // Read again, whatever the language.
    m_language.clear();
    reload();
}

// A catalog's strings in the language: English first (it carries the
// plural forms of counted strings), then the base language and the regional
// file over it (pt, then pt-BR), so a missing string falls back toward
// English one step at a time.
Translator::Strings Translator::read(const QString &directory) const {
    Strings strings;
    QStringList codes{"en", m_language.section('-', 0, 0), m_language};
    codes.removeDuplicates();
    for (const auto &code : codes) {
        QFile catalog(directory + "/" + code + ".json");
        if (!catalog.open(QIODevice::ReadOnly))
            continue;
        const QJsonObject object = QJsonDocument::fromJson(catalog.readAll()).object();
        for (auto it = object.begin(); it != object.end(); ++it)
            if ((it.value().isString() && !it.value().toString().isEmpty()) || it.value().isObject())
                strings.insert(it.key(), it.value());
    }
    return strings;
}

void Translator::reload() {
    const auto paths = m_watcher.files() + m_watcher.directories();
    if (!paths.isEmpty())
        m_watcher.removePaths(paths);

    // omarchy-language replaces the file, which drops it from the watch, and a
    // first pick creates it; watch its directory too and re-arm every time.
    const QString directory = QFileInfo(m_languageFile).absolutePath();
    if (QFileInfo::exists(directory))
        m_watcher.addPath(directory);
    if (QFileInfo::exists(m_languageFile))
        m_watcher.addPath(m_languageFile);

    // Catalogs are compiled in, so only a different language (or plugins'
    // catalogs, setPluginCatalogs) needs a reload.
    const QString language = readLanguage(m_languageFile);
    if (language == m_language)
        return;

    m_language = language;
    m_sources.clear();
    m_strings = read(m_catalogDirectory);
    m_files = filesIn(QStringLiteral(":/"), QDirIterator::NoIteratorFlags);
    m_plugins.clear();
    for (const auto &[id, dir] : m_pluginDirs)
        m_plugins.append({id, dir, filesIn(dir, QDirIterator::Subdirectories), read(dir + "/i18n")});
    // Dates, numbers, and Qt's own day and month names follow the language
    // too, and right-to-left languages mirror the window.
    const QLocale locale = formatLocale(language);
    QLocale::setDefault(locale);
    QGuiApplication::setLayoutDirection(locale.textDirection());
    emit changed();
}
