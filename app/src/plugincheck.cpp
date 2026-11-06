#include "plugins.h"
#include "theme.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMetaMethod>
#include <QMetaProperty>
#include <QRegularExpression>
#include <QHash>
#include <QSet>
#include <QUrl>

#include <algorithm>
#include <tuple>

namespace {
// A path a manifest names, kept inside the plugin's folder.
bool safeEntry(const QString &path) {
    return !path.isEmpty() && !path.startsWith('/') && !path.contains(QLatin1String(".."));
}

}

namespace {

// QML or JavaScript as code alone, by its tokens: its comments gone, and
// the words of its strings and regular expressions (a URL's "//", a
// "hidden:" said), their quotes left, but a string of one word (a name:
// "jpeg", "effect"); a template's ${…} is code again.
// Line breaks stay, so what follows is where it was.
QString codeOnly(const QString &source) {
    QString code;
    code.reserve(source.size());
    const qsizetype n = source.size();
    qsizetype i = 0;
    // The brace depth, and where each ${ of a template began.
    int depth = 0;
    QList<int> templates;
    // A "/" begins a regular expression where a value would: after an
    // operator, a bracket or nothing, or a word such as return; else it
    // divides.
    auto valueNext = [&code] {
        qsizetype k = code.size() - 1;
        while (k >= 0 && code[k].isSpace())
            --k;
        if (k < 0)
            return true;
        auto wordy = [](QChar c) { return c.isLetterOrNumber() || c == '_' || c == '$'; };
        if (wordy(code[k])) {
            qsizetype from = k;
            while (from > 0 && wordy(code[from - 1]))
                --from;
            static const QStringList before = {"return", "typeof", "case", "do",   "else",  "in",   "of",
                                               "new",    "delete", "void", "throw", "yield", "await"};
            return before.contains(code.mid(from, k - from + 1));
        }
        // After a value's ++ or --, it divides.
        if ((code[k] == '+' || code[k] == '-') && k > 0 && code[k - 1] == code[k]) {
            qsizetype j = k - 2;
            while (j >= 0 && code[j].isSpace())
                --j;
            if (j >= 0 && (wordy(code[j]) || code[j] == ')' || code[j] == ']'))
                return false;
        }
        return QStringLiteral("(,=:[!&|?{};+-*%<>~^").contains(code[k]);
    };
    // A template's words, up to its end or its next ${.
    auto templateWords = [&] {
        while (i < n) {
            const QChar c = source[i];
            if (c == '\\') {
                i += 2;
            } else if (c == '`') {
                code += c;
                ++i;
                return;
            } else if (c == '$' && i + 1 < n && source[i + 1] == '{') {
                code += QStringLiteral("${");
                i += 2;
                templates << depth++;
                return;
            } else {
                if (c == '\n')
                    code += c;
                ++i;
            }
        }
    };
    while (i < n) {
        const QChar c = source[i];
        const QChar next = i + 1 < n ? source[i + 1] : QChar();
        if (c == '/' && next == '/') {
            while (i < n && source[i] != '\n')
                ++i;
        } else if (c == '/' && next == '*') {
            code += ' ';
            for (i += 2; i < n && !(source[i] == '*' && i + 1 < n && source[i + 1] == '/'); ++i)
                if (source[i] == '\n')
                    code += '\n';
            i += 2;
        } else if (c == '"' || c == '\'') {
            // A string's words are not code; one word alone stays, as a
            // name (fetch's "jpeg", field's "effect").
            const qsizetype from = i + 1;
            bool word = true;
            for (++i; i < n && source[i] != c && source[i] != '\n'; ++i) {
                word &= source[i].isLetterOrNumber() || source[i] == '_';
                if (source[i] == '\\')
                    ++i;
            }
            code += c;
            if (word)
                code += source.mid(from, i - from);
            code += c;
            ++i;
        } else if (c == '`') {
            code += c;
            ++i;
            templateWords();
        } else if (c == '/' && valueNext()) {
            // Its flags follow, as a word.
            code += QStringLiteral("//");
            bool inClass = false;
            for (++i; i < n && source[i] != '\n'; ++i) {
                if (source[i] == '\\')
                    ++i;
                else if (source[i] == '[')
                    inClass = true;
                else if (source[i] == ']')
                    inClass = false;
                else if (source[i] == '/' && !inClass)
                    break;
            }
            ++i;
        } else if (c == '}' && !templates.isEmpty() && templates.last() == depth - 1) {
            code += c;
            ++i;
            --depth;
            templates.removeLast();
            templateWords();
        } else {
            if (c == '{')
                ++depth;
            else if (c == '}')
                --depth;
            code += c;
            ++i;
        }
    }
    return code;
}

// Where the bracket at `open` closes, in code alone; else its end.
qsizetype closing(const QString &code, qsizetype open) {
    const QChar in = code[open], out = in == '(' ? ')' : in == '[' ? ']' : '}';
    int depth = 0;
    for (qsizetype k = open; k < code.size(); ++k) {
        if (code[k] == in)
            ++depth;
        else if (code[k] == out && --depth == 0)
            return k;
    }
    return code.size() - 1;
}

// An object's own lines from its `{` at `open`: what is inside its
// children, calls and lists left out.
QString ownLines(const QString &code, qsizetype open) {
    const qsizetype end = closing(code, open);
    QString own;
    int depth = 0;
    for (qsizetype k = open; k <= end; ++k) {
        const QChar c = code[k];
        if (c == '{' || c == '(' || c == '[')
            ++depth;
        else if (c == '}' || c == ')' || c == ']')
            --depth;
        else if (depth == 1)
            own += c;
        if (depth != 1 && !own.endsWith('\n'))
            own += '\n';
    }
    return own;
}

// Defined below for the checker too; import paths need their quoted words.
QString withoutComments(const QString &source);

// What JavaScript can read of an object Pinhole gives: its properties,
// methods and signals.
QSet<QString> membersOf(const QMetaObject &meta) {
    QSet<QString> names;
    for (int i = 0; i < meta.propertyCount(); ++i)
        names << QString::fromLatin1(meta.property(i).name());
    for (int i = 0; i < meta.methodCount(); ++i)
        names << QString::fromLatin1(meta.method(i).name());
    return names;
}

// The one of `names` nearest `word` (a misspelling's), if one is near.
QString nearest(const QString &word, const QStringList &names) {
    const auto distance = [](const QString &a, const QString &b) {
        std::vector<int> row(size_t(b.size()) + 1);
        for (qsizetype j = 0; j <= b.size(); ++j)
            row[size_t(j)] = int(j);
        for (qsizetype i = 1; i <= a.size(); ++i) {
            int diagonal = row[0];
            row[0] = int(i);
            for (qsizetype j = 1; j <= b.size(); ++j) {
                const int up = row[size_t(j)];
                row[size_t(j)] = std::min({up + 1, row[size_t(j) - 1] + 1,
                                           diagonal + (a[i - 1].toLower() == b[j - 1].toLower() ? 0 : 1)});
                diagonal = up;
            }
        }
        return row[size_t(b.size())];
    };
    QString best;
    int least = std::max(2, int(word.size()) / 3) + 1;
    for (const QString &name : names)
        if (const int d = distance(word, name); d < least) {
            least = d;
            best = name;
        }
    return best;
}

QString didYouMean(const QString &word, const QStringList &names) {
    QString near = nearest(word, names);
    // Or the one it begins (select, selection).
    for (const QString &name : names)
        if (near.isEmpty() && word.size() >= 3 && name.startsWith(word, Qt::CaseInsensitive))
            near = name;
    return near.isEmpty() ? QString() : QStringLiteral(" (%1?)").arg(near);
}

bool shellButtonCheckable(const QString &source, const QString &code, const QString &path) {
    static const QRegularExpression imports(
        QStringLiteral("^\\s*import\\s+Pinhole\\.Plugin(?:\\s+[0-9.]+)?(?:\\s+as\\s+(\\w+))?\\s*;?\\s*$"),
        QRegularExpression::MultilineOption);
    static const QRegularExpression localImports(
        QStringLiteral("^\\s*import\\s+[\"']([^\"']+)[\"']\\s*;?\\s*$"), QRegularExpression::MultilineOption);
    static const QRegularExpression inlineButton(QStringLiteral("\\bcomponent\\s+ShellButton\\s*:"));
    static const QRegularExpression checkable(QStringLiteral("(^|[;\\s])checkable\\s*:"));
    static const QRegularExpression ownProperty(QStringLiteral("\\bproperty\\s+\\w+\\s+checkable\\s*:"));
    const QDir here = QFileInfo(path).dir();
    bool shadowed = QFileInfo::exists(here.filePath("ShellButton.qml")) || code.contains(inlineButton);
    for (auto local = localImports.globalMatch(withoutComments(source)); local.hasNext();) {
        if (QFileInfo::exists(here.filePath(local.next().captured(1) + "/ShellButton.qml")))
            shadowed = true;
    }
    for (auto imported = imports.globalMatch(code); imported.hasNext();) {
        const QString alias = imported.next().captured(1);
        if (alias.isEmpty() && shadowed) continue;
        const QString type = alias.isEmpty() ? QStringLiteral("ShellButton")
            : QRegularExpression::escape(alias) + QStringLiteral("\\s*\\.\\s*ShellButton");
        const QRegularExpression button(QStringLiteral("(?<![\\w.])") + type + QStringLiteral("\\s*\\{"));
        for (auto match = button.globalMatch(code); match.hasNext();) {
            const auto found = match.next();
            // A different namespace may put spaces around its dot too.
            qsizetype before = found.capturedStart();
            while (before > 0 && code[before - 1].isSpace()) --before;
            if (before > 0 && code[before - 1] == '.') continue;
            const QString own = ownLines(code, found.capturedEnd() - 1);
            if (own.contains(checkable) && !own.contains(ownProperty)) return true;
        }
    }
    return false;
}

} // namespace

QMap<int, QStringList> Plugins::apiUses(const QString &dir) {
    // Each by the API that brought it, and the words that use it: pinhole's
    // own, or what a plugin's root sets. Anything added to what a plugin is
    // given goes here, under the API it is released in.
    static const QList<std::pair<int, QString>> order = {
        {2, "promises"},      {2, "pinhole.settings"}, {2, "pinhole.notify"},   {2, "pinhole.warn"},
        {2, "pinhole.api"},   {2, "pinhole.coveredBy"}, {2, "pinhole.dismissed"}, {2, "hidden"},
        {2, "avgColor"},      {2, "library.field"},    {2, "pinhole.isHidden"}, {2, "run's output"},
        {2, "pinhole.keepAwake"}, {3, "marks"}, {3, "pinhole.state"}, {3, "pinhole.cache"},
        {3, "viewerLayer"}, {3, "an action's order, hint, busyDelay, textColour or complain"}, {3, "fetch's options"},
        {3, "fetch's loop, bounce, long_exposure or jpeg"}, {3, "a photo's effect"},
        {3, "pinhole.setFavorite"}, {3, "library.revision"}, {3, "sources in the manifest"}, {3, "PluginSource"},
        {3, "pinhole.sources"}, {3, "pinhole.chooseFolder"}, {3, "pinhole.dropStep"}, {4, "a photo's thumbPath"},
        {4, "pinhole.display"}, {4, "viewer.hdr"}, {4, "pinhole.srgbProfile"}, {4, "SdrVideo"}, {4, "Scroller"},
        {5, "ScrollList, ScrollGrid or ScrollArea"}, {5, "PinchZoom"}, {5, "Panel or Card"}, {5, "Choices"},
        {5, "pinhole.query"}, {5, "pinhole.picturePath"}, {5, "VideoView"}, {6, "holdSdr"}, {7, "ShellButton.checkable"},
        {8, "pinhole.openPhotoDirect"}, {9, "pinhole.openPhotos"}, {10, "pinhole.selectionIds"}, {10, "pinhole.screenshots"},
        {11, "comeBack, away or returned"}, {11, "workers in the manifest"}, {11, "PhotoTiles"}, {11, "PanelHeader"}, {11, "SettingNote or SettingSwitch"}, {11, "Choices.numbered"}, {11, "pinhole.keyboardInUse"}, {12, "pinhole.copyText, input or hasKeys"}, {12, "pinhole.workers or workerSaid"}};
    static const QList<std::pair<QString, QRegularExpression>> words = {
        // Its root's marks, each a PluginMark.
        {"marks", QRegularExpression(QStringLiteral("\\bPluginMark\\s*\\{"))},
        {"promises", QRegularExpression(QStringLiteral("\\.\\s*then\\s*\\("))},
        {"pinhole.settings", QRegularExpression(QStringLiteral("\\bpinhole\\s*\\.\\s*settings\\b"))},
        {"pinhole.notify", QRegularExpression(QStringLiteral("\\bpinhole\\s*\\.\\s*notify\\b"))},
        {"pinhole.warn", QRegularExpression(QStringLiteral("\\bpinhole\\s*\\.\\s*warn\\b"))},
        {"pinhole.api", QRegularExpression(QStringLiteral("\\bpinhole\\s*\\.\\s*api\\b"))},
        {"pinhole.coveredBy", QRegularExpression(QStringLiteral("\\bpinhole\\s*\\.\\s*coveredBy\\b"))},
        {"pinhole.dismissed", QRegularExpression(QStringLiteral("\\bpinhole\\s*\\.\\s*dismissed\\b"))},
        // A photo's, as library.item gives it.
        {"avgColor", QRegularExpression(QStringLiteral("\\.\\s*avgColor\\b"))},
        // pinhole.library's, or by a name of the plugin's for it.
        {"library.field", QRegularExpression(QStringLiteral("\\blibrary\\s*\\.\\s*field\\s*\\("))},
        {"pinhole.isHidden",
         QRegularExpression(QStringLiteral("\\bpinhole\\s*\\.\\s*(isHidden|hiddenCount|hiddenChanged)\\b"))},
        {"pinhole.keepAwake", QRegularExpression(QStringLiteral("\\bpinhole\\s*\\.\\s*keepAwake\\b"))},
        {"pinhole.state", QRegularExpression(QStringLiteral("\\bpinhole\\s*\\.\\s*state\\b"))},
        {"pinhole.cache", QRegularExpression(QStringLiteral("\\bpinhole\\s*\\.\\s*cache\\b"))},
        {"viewerLayer", QRegularExpression(QStringLiteral("\\bviewerLayer\\s*:"))},
        {"pinhole.setFavorite", QRegularExpression(QStringLiteral("\\bpinhole\\s*\\.\\s*setFavorite\\b"))},
        // Its root's sources, each a PluginSource.
        {"PluginSource", QRegularExpression(QStringLiteral("\\bPluginSource\\s*\\{"))},
        {"pinhole.sources", QRegularExpression(QStringLiteral("\\bpinhole\\s*\\.\\s*sources\\b"))},
        {"pinhole.chooseFolder", QRegularExpression(QStringLiteral("\\bpinhole\\s*\\.\\s*chooseFolder\\b"))},
        {"pinhole.dropStep", QRegularExpression(QStringLiteral("\\bpinhole\\s*\\.\\s*dropStep\\b"))},
        // pinhole.library's, or by a name of the plugin's for it.
        {"library.revision", QRegularExpression(QStringLiteral("\\blibrary\\s*\\.\\s*revision\\b"))},
        {"an action's order, hint, busyDelay, textColour or complain", QRegularExpression(QStringLiteral("\\.\\s*complain\\s*\\("))},
        // The kinds fetch has had since API 3, named where they are not in
        // the call itself (Background's `kind: "loop"`).
        {"fetch's loop, bounce, long_exposure or jpeg",
         QRegularExpression(QStringLiteral("\\bkind\\s*[:=]\\s*[\"'](loop|bounce|long_exposure|jpeg)[\"']"))},
        {"a photo's effect", QRegularExpression(QStringLiteral("\\bfield\\s*\\(\\s*[\"']effect[\"']\\s*\\)"))},
        // The thumbnail's file beside its URL. (image://file, API 4's too,
        // is in a string, whose words are not looked at.)
        {"a photo's thumbPath", QRegularExpression(QStringLiteral("\\.\\s*thumbPath\\b"))},
        {"pinhole.display", QRegularExpression(QStringLiteral("\\bpinhole\\s*\\.\\s*display\\b"))},
        // pinhole.viewer's, or by a name of the plugin's for it.
        {"viewer.hdr", QRegularExpression(QStringLiteral("\\bviewer\\s*\\.\\s*hdr\\b"))},
        {"SdrVideo", QRegularExpression(QStringLiteral("\\bSdrVideo\\s*\\{"))},
        {"pinhole.srgbProfile", QRegularExpression(QStringLiteral("\\bpinhole\\s*\\.\\s*srgbProfile\\b"))},
        {"Scroller", QRegularExpression(QStringLiteral("\\bScroller\\s*\\{"))},
        {"ScrollList, ScrollGrid or ScrollArea", QRegularExpression(QStringLiteral("\\bScroll(List|Grid|Area)\\s*\\{"))},
        {"PinchZoom", QRegularExpression(QStringLiteral("\\bPinchZoom\\s*\\{"))},
        {"Panel or Card", QRegularExpression(QStringLiteral("\\b(Panel|Card)\\s*\\{"))},
        {"Choices", QRegularExpression(QStringLiteral("\\bChoices\\s*\\{"))},
        {"pinhole.query", QRegularExpression(QStringLiteral("\\bpinhole\\s*\\.\\s*query\\s*\\("))},
        {"pinhole.picturePath", QRegularExpression(QStringLiteral("\\bpinhole\\s*\\.\\s*picturePath\\b"))},
        {"pinhole.openPhotoDirect", QRegularExpression(QStringLiteral("\\bpinhole\\s*\\.\\s*openPhotoDirect\\b"))},
        {"pinhole.openPhotos", QRegularExpression(QStringLiteral("\\bpinhole\\s*\\.\\s*openPhotos\\b"))},
        {"pinhole.selectionIds", QRegularExpression(QStringLiteral("\\bpinhole\\s*\\.\\s*selectionIds\\b"))},
        {"pinhole.screenshots", QRegularExpression(QStringLiteral("\\bpinhole\\s*\\.\\s*screenshots\\b"))},
        {"PhotoTiles", QRegularExpression(QStringLiteral("\\bPhotoTiles\\s*\\{"))},
        {"PanelHeader", QRegularExpression(QStringLiteral("\\bPanelHeader\\s*\\{"))},
        {"SettingNote or SettingSwitch", QRegularExpression(QStringLiteral("\\bSetting(Note|Switch)\\s*\\{"))},
        {"Choices.numbered", QRegularExpression(QStringLiteral("\\bnumbered\\s*:"))},
        {"pinhole.keyboardInUse", QRegularExpression(QStringLiteral("\\bpinhole\\s*\\.\\s*keyboardInUse\\b"))},
        {"pinhole.workers or workerSaid", QRegularExpression(QStringLiteral("\\bpinhole\\s*\\.\\s*workers\\b|\\bonWorkerSaid\\b|\\bonWorkersChanged\\b"))},
        {"pinhole.copyText, input or hasKeys", QRegularExpression(QStringLiteral("\\bpinhole\\s*\\.\\s*(copyText|input|hasKeys)\\b"))},
        {"comeBack, away or returned", QRegularExpression(QStringLiteral("\\bcomeBack\\s*:|\\bpinhole\\s*\\.\\s*away\\b|\\bonReturned\\b"))},
        {"VideoView", QRegularExpression(QStringLiteral("\\bVideoView\\s*\\{"))},
        // Its root's, kept at SDR while true.
        {"holdSdr", QRegularExpression(QStringLiteral("\\bholdSdr\\s*:"))},
    };
    // pinhole's signals, in a Connections to it.
    static const QRegularExpression connections(QStringLiteral("\\bConnections\\s*\\{"));
    static const QRegularExpression toPinhole(
        QStringLiteral("\\btarget\\s*:\\s*([\\w$]+\\s*\\.\\s*)*pinhole\\s*[;}\\n]"));
    static const QList<std::pair<QString, QRegularExpression>> handlers = {
        {"pinhole.coveredBy", QRegularExpression(QStringLiteral("\\bonCoveredByChanged\\b"))},
        {"pinhole.dismissed", QRegularExpression(QStringLiteral("\\bonDismissed\\b"))},
        {"pinhole.isHidden", QRegularExpression(QStringLiteral("\\bonHiddenChanged\\b"))},
    };
    // run's options, in its call.
    static const QRegularExpression run(QStringLiteral("\\bpinhole\\s*\\.\\s*run\\s*\\("));
    static const QRegularExpression output(QStringLiteral("\\boutput\\s*:"));
    // fetch's, in its call.
    static const QRegularExpression fetch(QStringLiteral("\\bpinhole\\s*\\.\\s*fetch\\s*\\("));
    static const QRegularExpression fetchOptions(QStringLiteral("\\bstream\\s*:"));
    static const QRegularExpression fetchKinds(QStringLiteral("[\"'](loop|bounce|long_exposure|jpeg)[\"']"));
    // A photo's effect, as library.item gives it: whatever is before the
    // dot but a layer (Qt Quick's own `layer.effect`).
    static const QRegularExpression effect(QStringLiteral("(?:\\b([\\w$]+)|[\\])])\\s*\\.\\s*effect\\b"));
    // A PluginAction's own lines that say its order, hint, busyDelay or
    // textColour.
    static const QRegularExpression action(QStringLiteral("\\bPluginAction\\s*\\{"));
    static const QRegularExpression orderOrHint(QStringLiteral("(^|[;\\s])(order|hint|busyDelay|textColour)\\s*:"));
    // A file's root object, its id, and its hidden set.
    static const QRegularExpression root(QStringLiteral("([\\w.]+)\\s*\\{"));
    static const QRegularExpression rootId(QStringLiteral("(^|[;\\s])id\\s*:\\s*([\\w$]+)"));
    static const QRegularExpression hiddenSet(QStringLiteral("(^|[;\\s])hidden\\s*:"));

    QSet<QString> uses;
    QDirIterator it(dir, {"*.qml", "*.js"}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        QFile file(it.next());
        if (file.fileName().contains(QLatin1String("/target/")) || !file.open(QIODevice::ReadOnly))
            continue;
        const QString source = QString::fromUtf8(file.readAll());
        const QString code = codeOnly(source);
        if (file.fileName().endsWith(QLatin1String(".qml")) && shellButtonCheckable(source, code, file.fileName()))
            uses << QStringLiteral("ShellButton.checkable");
        for (const auto &[name, pattern] : words)
            if (code.contains(pattern))
                uses << name;
        for (auto match = connections.globalMatch(code); match.hasNext();) {
            const qsizetype open = match.next().capturedEnd() - 1;
            const QString block = code.mid(open, closing(code, open) - open + 1);
            if (block.contains(toPinhole))
                for (const auto &[name, pattern] : handlers)
                    if (block.contains(pattern))
                        uses << name;
        }
        for (auto match = run.globalMatch(code); match.hasNext();) {
            const qsizetype open = match.next().capturedEnd() - 1;
            if (code.mid(open, closing(code, open) - open + 1).contains(output))
                uses << QStringLiteral("run's output");
        }
        for (auto match = fetch.globalMatch(code); match.hasNext();) {
            const qsizetype open = match.next().capturedEnd() - 1;
            const QString call = code.mid(open, closing(code, open) - open + 1);
            if (call.contains(fetchOptions))
                uses << QStringLiteral("fetch's options");
            if (call.contains(fetchKinds))
                uses << QStringLiteral("fetch's loop, bounce, long_exposure or jpeg");
        }
        for (auto match = effect.globalMatch(code); match.hasNext();)
            if (match.next().captured(1) != QLatin1String("layer"))
                uses << QStringLiteral("a photo's effect");
        for (auto match = action.globalMatch(code); match.hasNext();) {
            const qsizetype open = match.next().capturedEnd() - 1;
            if (ownLines(code, open).contains(orderOrHint))
                uses << QStringLiteral("an action's order, hint, busyDelay, textColour or complain");
        }
        // A PinholePlugin root's hidden: set where it is, or by its id.
        const auto top = root.match(code);
        if (!file.fileName().endsWith(QLatin1String(".qml")) || !top.hasMatch()
            || top.captured(1).section('.', -1) != QLatin1String("PinholePlugin"))
            continue;
        const QString own = ownLines(code, top.capturedEnd() - 1);
        const auto id = rootId.match(own);
        if (own.contains(hiddenSet)
            || (id.hasMatch()
                && code.contains(QRegularExpression("\\b" + QRegularExpression::escape(id.captured(2)) + "\\s*\\.\\s*hidden\\s*=(?!=)"))))
            uses << QStringLiteral("hidden");
    }
    // Sources and workers of its own, in its manifest.
    if (QFile manifest(dir + "/manifest.json"); manifest.open(QIODevice::ReadOnly)) {
        const QJsonObject declared = QJsonDocument::fromJson(manifest.readAll()).object();
        if (declared.contains(QLatin1String("sources")))
            uses << QStringLiteral("sources in the manifest");
        if (declared.contains(QLatin1String("workers")))
            uses << QStringLiteral("workers in the manifest");
    }
    QMap<int, QStringList> byApi;
    for (const auto &[version, name] : order)
        if (uses.contains(name))
            byApi[version] << name;
    return byApi;
}

namespace {

// QML or JavaScript with its comments gone and its strings kept, line
// breaks where they were.
QString withoutComments(const QString &source) {
    QString out;
    out.reserve(source.size());
    const qsizetype n = source.size();
    QChar quote;
    for (qsizetype i = 0; i < n; ++i) {
        const QChar c = source[i];
        if (!quote.isNull()) {
            out += c;
            if (c == '\\' && i + 1 < n)
                out += source[++i];
            else if (c == quote || (c == '\n' && quote != '`'))
                quote = QChar();
            continue;
        }
        if (c == '"' || c == '\'' || c == '`') {
            quote = c;
            out += c;
        } else if (c == '/' && i + 1 < n && source[i + 1] == '/') {
            while (i < n && source[i] != '\n')
                ++i;
            if (i < n)
                out += '\n';
        } else if (c == '/' && i + 1 < n && source[i + 1] == '*') {
            for (i += 2; i + 1 < n && !(source[i] == '*' && source[i + 1] == '/'); ++i)
                if (source[i] == '\n')
                    out += '\n';
            ++i;
        } else {
            out += c;
        }
    }
    return out;
}

int lineAt(const QString &text, qsizetype position) { return int(text.left(position).count('\n')) + 1; }

// The text with the words given to qsTr() blanked, every character in its
// place: words shown ("Kept in ~/Pictures/Pinhole") are not code.
QString withoutShownWords(const QString &text) {
    static const QRegularExpression tr(QStringLiteral("\\bqsTr(?:anslate|Id)?\\s*\\("));
    QString out = text;
    for (auto m = tr.globalMatch(text); m.hasNext();) {
        qsizetype k = m.next().capturedEnd();
        int depth = 1;
        QChar quote;
        while (k < text.size() && depth > 0) {
            const QChar c = text[k];
            if (!quote.isNull()) {
                if (c == '\\' && k + 1 < text.size()) {
                    out[k] = out[k + 1] = ' ';
                    k += 2;
                    continue;
                }
                if (c == quote)
                    quote = QChar();
                else if (c != '\n')
                    out[k] = ' ';
            } else if (c == '"' || c == '\'' || c == '`') {
                quote = c;
            } else if (c == '(') {
                ++depth;
            } else if (c == ')') {
                --depth;
            }
            ++k;
        }
    }
    return out;
}

// The type of the object whose `{` holds `position` ("PinholePlugin",
// "Item"), without its module's name, and where that `{` is; -1 if none.
std::pair<QString, qsizetype> enclosingObject(const QString &code, qsizetype position) {
    static const QRegularExpression type(QStringLiteral("(?:\\w+\\s*\\.\\s*)?(\\w+)\\s*$"));
    int depth = 0;
    for (qsizetype k = position - 1; k >= 0; --k) {
        const QChar c = code[k];
        if (c == '}' || c == ')' || c == ']') {
            ++depth;
        } else if (c == '{' || c == '(' || c == '[') {
            if (depth > 0) {
                --depth;
            } else if (c == '{') {
                const qsizetype from = std::max<qsizetype>(0, k - 80);
                return {type.match(code.mid(from, k - from)).captured(1), k};
            }
        }
    }
    return {QString(), -1};
}

// The handler a statement at `position` is in, if any: "onTriggered",
// "onWidthChanged"…, or "" in a function or a binding of its own.
QString handlerAround(const QString &code, qsizetype position) {
    static const QRegularExpression handler(
        QStringLiteral("(?:\\b(on[A-Z]\\w*)\\s*:\\s*(?:(?:\\w+|\\([^()]*\\))\\s*=>\\s*)?|\\bfunction\\s+(on[A-Z]\\w*)\\s*\\([^()]*\\)\\s*)$"));
    static const QRegularExpression objectStart(QStringLiteral("\\b[A-Z][\\w.]*\\s*$"));
    // The handler's own statement: `onXChanged: pinhole.setSetting(…)`,
    // on its line or after an object's `{`.
    qsizetype from = position;
    while (from > 0 && code[from - 1] != '\n' && code[from - 1] != '{' && code[from - 1] != ';')
        --from;
    if (const auto m = QRegularExpression(QStringLiteral("^\\s*(on[A-Z]\\w*)\\s*:")).match(code.mid(from, position - from));
        m.hasMatch())
        return m.captured(1);
    int depth = 0;
    for (qsizetype k = position - 1; k >= 0; --k) {
        if (code[k] == '}' || code[k] == ')' || code[k] == ']') {
            ++depth;
        } else if (code[k] == '(' || code[k] == '[') {
            depth = std::max(0, depth - 1);
        } else if (code[k] == '{') {
            if (depth > 0) {
                --depth;
                continue;
            }
            const QString before = code.mid(std::max<qsizetype>(0, k - 160), std::min<qsizetype>(k, 160));
            if (const auto m = handler.match(before); m.hasMatch())
                return m.captured(1).isEmpty() ? m.captured(2) : m.captured(1);
            if (objectStart.match(before).hasMatch())
                return {};
        }
    }
    return {};
}

} // namespace

QString Plugins::refusedRun(const QString &program, const QStringList &arguments) {
    static const QStringList refused = {"sqlite3", "wl-copy", "wl-paste", "xclip", "xsel", "xdg-user-dir"};
    // Run what they are given: a program and its arguments.
    static const QStringList wrappers = {"env",   "nice",   "nohup", "setsid", "timeout", "stdbuf", "ionice", "flock",
                                         "xargs", "time",   "sudo",  "doas",   "uwsm",    "uwsm-app", "app2unit",
                                         "systemd-run", "busybox"};
    // Run a line of their own: its words looked at.
    static const QStringList shells = {"sh", "bash", "dash", "zsh", "ksh", "mksh", "fish", "busybox"};
    static const QRegularExpression named(
        QStringLiteral("(?<![\\w.-])(sqlite3|wl-copy|wl-paste|xclip|xsel|xdg-user-dir)(?![\\w.-])"));
    auto why = [](const QString &name) {
        if (name == QLatin1String("sqlite3"))
            return tr("sqlite3 is refused: it opens the library while the service writes it. Use pinhole.query");
        if (name == QLatin1String("xdg-user-dir"))
            return tr("xdg-user-dir is refused: in a self-test it finds the real Pictures. Use pinhole.picturePath");
        return tr("%1 is refused: it sets the session's clipboard, a self-test's too. Use pinhole.copyFile").arg(name);
    };
    const QString base = QFileInfo(program).fileName();
    if (refused.contains(base))
        return why(base);
    if (wrappers.contains(base) || shells.contains(base))
        for (const QString &argument : arguments)
            if (const QString name = QFileInfo(argument).fileName(); refused.contains(name))
                return why(name);
    if (shells.contains(base))
        for (const QString &argument : arguments)
            if (const auto m = named.match(argument); m.hasMatch())
                return why(m.captured(1));
    return {};
}

Plugins::Findings Plugins::footguns(const QString &dir) {
    Findings found;
    // Each by the words that give it away in its code, strings and
    // comments aside, and what to use instead.
    struct Rule {
        QRegularExpression pattern;
        QString what;
        // Looked for in its strings too (codeOnly drops their words).
        bool inStrings = false;
    };
    static const QList<Rule> rules = {
        {QRegularExpression(QStringLiteral("\\bListView\\s*\\{")),
         QStringLiteral("a ListView scrolls not as the grid does, and lets the wheel through to the photos at its ends: use ScrollList")},
        {QRegularExpression(QStringLiteral("\\bGridView\\s*\\{")),
         QStringLiteral("a GridView scrolls not as the grid does, and lets the wheel through to the photos at its ends: use ScrollGrid")},
        {QRegularExpression(QStringLiteral("\\b(Flickable|ScrollView)\\s*\\{")),
         QStringLiteral("a %1 scrolls not as the grid does, and lets the wheel through to the photos at its ends: use ScrollArea")},
        {QRegularExpression(QStringLiteral("\\b(TableView|TreeView|HorizontalHeaderView|VerticalHeaderView)\\s*\\{")),
         QStringLiteral("a %1 scrolls not as the grid does, and lets the wheel through to the photos at its ends: use ScrollGrid "
                        "or ScrollArea")},
        {QRegularExpression(QStringLiteral("\\b(PathView|Tumbler|SwipeView)\\s*\\{")),
         QStringLiteral("a %1 flicks not as the grid does, and lets the wheel through to the photos: use ScrollList, or a "
                        "Panel's steps")},
        {QRegularExpression(QStringLiteral("\\b(PinchHandler|PinchArea|MultiPointTouchArea)\\s*\\{")),
         QStringLiteral("a %1 zooms not as the grid does, and lets a touchpad's pinch through to the photos: use PinchZoom")},
        // QML made from a string: what it holds is never looked at.
        {QRegularExpression(QStringLiteral("\\bQt\\s*\\.\\s*createQmlObject\\s*\\(")),
         QStringLiteral("Qt.createQmlObject makes QML from a string, which check cannot look over: put it in a file of the "
                        "plugin's, made with a Loader or Qt.createComponent")},
        // Video: Qt's own output shows HDR wrong.
        {QRegularExpression(QStringLiteral("\\bVideoOutput\\s*\\{")),
         QStringLiteral("a VideoOutput shows an HDR video (an iPhone's) flat and grey, or its brightest colours broken: use "
                        "VideoView")},
        {QRegularExpression(QStringLiteral("\\bWheelHandler\\s*\\{")),
         QStringLiteral("a WheelHandler: use ScrollList, ScrollGrid or ScrollArea to scroll, a Panel's steps to step")},
        {QRegularExpression(QStringLiteral("\\bonWheel\\s*:")),
         QStringLiteral("a MouseArea's onWheel: a Panel or a Card under the controls takes the wheel, ScrollList, ScrollGrid "
                        "and ScrollArea scroll, a Panel's steps step")},
        // A program, by its name anywhere in a string: on its own, by its
        // path, in a shell's line (`sh -c "…"`, backticks); or put together.
        // (pinhole.run refuses them too, whatever made the name.)
        {QRegularExpression(QStringLiteral("[\"'`][^\"'`\\n]*(?<![\\w.-])(sqlite3)(?![\\w.-])|\\b(sqlite)[\"'`]\\s*\\+\\s*[\"'`]3")),
         QStringLiteral("sqlite3 on the library opens it while the service writes it (\"database is locked\"): use pinhole.query"),
         true},
        {QRegularExpression(QStringLiteral("[\"'`][^\"'`\\n]*(?<![\\w.-])(wl-copy|wl-paste|xclip|xsel)(?![\\w.-])")),
         QStringLiteral("%1 sets the session's clipboard, a self-test's too: use pinhole.copyText for words, "
                        "pinhole.copyFile for a file"), true},
        {QRegularExpression(QStringLiteral("[\"'`][^\"'`\\n]*(?<![\\w.-])(xdg-user-dir)(?![\\w.-])")),
         QStringLiteral("Pictures found by hand is the real one in a self-test: use pinhole.picturePath, or pinhole.picturesDir"),
         true},
        // A key of what it keeps, changed inside: a copy is changed, and
        // nothing is kept or told.
        {QRegularExpression(QStringLiteral("\\bpinhole\\s*\\.\\s*(state|cache|settings)\\s*(?:\\.\\s*\\w+|\\[[^\\]\\n]+\\])"
                                           "\\s*(?:\\.\\s*\\w+|\\[[^\\]\\n]+\\])+\\s*(?:[-+*/]?=(?!=)|\\+\\+|--)")),
         QStringLiteral("pinhole.%1's key changed inside is a copy changed: nothing is kept, and nothing bound to it hears. "
                        "Copy the key, change the copy, assign it whole")},
        {QRegularExpression(QStringLiteral("\\bpinhole\\s*\\.\\s*(state|cache|settings)\\s*(?:\\.\\s*\\w+|\\[[^\\]\\n]+\\])"
                                           "\\s*(?:\\.\\s*\\w+|\\[[^\\]\\n]+\\])*\\s*\\.\\s*(?:push|pop|shift|unshift|splice|sort|reverse|fill)\\s*\\(")),
         QStringLiteral("pinhole.%1's key changed in place is a copy changed: nothing is kept, and nothing bound to it hears. "
                        "Copy the key, change the copy, assign it whole")},
        {QRegularExpression(QStringLiteral("\\bdelete\\s+pinhole\\s*\\.\\s*(state|cache|settings)\\s*(?:\\.\\s*\\w+|\\[[^\\]\\n]+\\])"
                                           "\\s*(?:\\.\\s*\\w+|\\[[^\\]\\n]+\\])+")),
         QStringLiteral("deleting inside pinhole.%1's key changes a copy: nothing is kept. Copy the key, delete from the "
                        "copy, assign it whole")},
        // A property QML reads as a signal's handler.
        {QRegularExpression(QStringLiteral("\\bproperty\\s+[\\w<>.]+\\s+(on[A-Z]\\w*)")),
         QStringLiteral("a property named %1 is read by QML as a signal's handler: it never holds a value. Name it otherwise")},
        {QRegularExpression(QStringLiteral("\\bPicturesLocation\\b")),
         QStringLiteral("Pictures found by hand is the real one in a self-test: use pinhole.picturePath, or pinhole.picturesDir")},
    };
    // In its strings: Pictures by its path, whole or a part to build one
    // with ("/Pictures/Pinhole/").
    static const QRegularExpression picturesPath(
        QStringLiteral("[\"'`][^\"'`\\n]*?((?:~|\\$HOME|\\$\\{HOME\\}|/home/[^/\"'`]+)?/Pictures)(?![\\w-])"));
    // QtMultimedia's Video, a VideoOutput of its own.
    static const QRegularExpression multimedia(QStringLiteral("^\\s*import\\s+QtMultimedia\\b"), QRegularExpression::MultilineOption);
    static const QRegularExpression video(QStringLiteral("\\bVideo\\s*\\{"));
    // A MouseArea over the whole window: the wheel and a pinch go through
    // it to the photos.
    static const QRegularExpression mouseArea(QStringLiteral("\\bMouseArea\\s*\\{"));
    static const QRegularExpression fills(QStringLiteral("\\banchors\\s*\\.\\s*fill\\s*:\\s*parent\\b"));
    // The help's words: each entry's second string.
    static const QRegularExpression helpList(QStringLiteral("\\bhelp\\s*:\\s*\\["));
    static const QRegularExpression helpEntry(QStringLiteral("\\[\\s*(?:\"(?:[^\"\\\\]|\\\\.)*\"|[^,\\]]*)\\s*,\\s*\"((?:[^\"\\\\]|\\\\.)*)\""));
    // Words shown as they are, and keys as Qt writes them in what is shown.
    static const QRegularExpression said(QStringLiteral("\\bqsTr\\s*\\(\\s*(?:\"((?:[^\"\\\\]|\\\\.)*)\"|'((?:[^'\\\\]|\\\\.)*)')"));
    // Words put together inside qsTr: none of them is found to translate.
    static const QRegularExpression joined(QStringLiteral("\\bqsTr\\s*\\(\\s*([\"'])(?:(?!\\1)[^\\\\]|\\\\.)*\\1\\s*\\+"));
    // What Pinhole gives a plugin, by the names it has (a misspelt one is
    // undefined, and fails only as it runs).
    static const QSet<QString> hostMembers = membersOf(PluginHost::staticMetaObject);
    static const QSet<QString> libraryMembers = membersOf(PluginLibrary::staticMetaObject);
    static const QSet<QString> themeMembers = membersOf(Theme::staticMetaObject);
    static const QSet<QString> viewerMembers = membersOf(PluginViewer::staticMetaObject);
    static const QRegularExpression viewerUse(QStringLiteral("(?<![\\w$])pinhole\\s*\\.\\s*viewer\\s*\\.\\s*([A-Za-z_$][\\w$]*)"));
    static const QRegularExpression hostUse(QStringLiteral("(?<![\\w$])pinhole\\s*\\.\\s*([A-Za-z_$][\\w$]*)"));
    static const QRegularExpression libraryUse(QStringLiteral("(?<![\\w$])pinhole\\s*\\.\\s*library\\s*\\.\\s*([A-Za-z_$][\\w$]*)"));
    static const QRegularExpression themeUse(QStringLiteral("(?<![\\w$.])theme\\s*\\.\\s*([A-Za-z_$][\\w$]*)"));
    static const QRegularExpression ownTheme(QStringLiteral("\\bid\\s*:\\s*theme\\b|\\bproperty\\s+\\w+\\s+theme\\b|\\b(?:var|let|const)\\s+theme\\b"));
    static const QRegularExpression fieldUse(QStringLiteral("\\bfield\\s*\\(\\s*[\"']([^\"']*)[\"']"));
    // Menus that open over the window, outside the plugin's Panel: actions
    // go on controls one can see (and a touchpad's two-finger tap is no
    // menu's either).
    static const QRegularExpression controls(QStringLiteral("^[ \\t]*import\\s+QtQuick\\.Controls\\b"), QRegularExpression::MultilineOption);
    static const QRegularExpression popup(QStringLiteral("\\b(Menu|Popup|ComboBox|Dialog|Drawer)\\s*\\{"));
    static const QRegularExpression contextMenu(QStringLiteral("\\bContextMenu\\s*[.{]"));
    // A text field's copy(): the session's clipboard, a self-test's too.
    static const QRegularExpression fieldCopy(QStringLiteral("\\.\\s*copy\\s*\\(\\s*\\)"));
    // A file a Loader, Image or createComponent takes, by its path.
    static const QRegularExpression fileUse(QStringLiteral("\\b(?:source\\s*:|setSource\\s*\\(|createComponent\\s*\\()\\s*[\"']([^\"']+)[\"']"));
    // A Command and the layer that has its keys.
    static const QRegularExpression command(QStringLiteral("\\bCommand\\s*\\{"));
    static const QRegularExpression keyLayer(QStringLiteral("(?:^|[;\\s])keyLayer\\s*:\\s*([^;\\n]*)"));
    static const QRegularExpression shown(QStringLiteral(
        "(?:^|[;{\\s])(text|title|helpText|placeholder|noun)\\s*:\\s*\"((?:[^\"\\\\]|\\\\.)*)\"|"
        "\\b(?:notify|complain)\\s*\\(\\s*\"((?:[^\"\\\\]|\\\\.)*)\""));
    static const QRegularExpression wordy(QStringLiteral("[A-Za-z]{3,}"));
    static const QRegularExpression brand(QStringLiteral("^[a-z]+[A-Z]\\w*$"));
    static const QRegularExpression rawKey(QStringLiteral("\\b(?:(?:Shift|Ctrl|Alt|Meta)\\+\\w+|Return|Escape|PgUp|PgDown|Backspace)\\b"));
    // A setting written in a handler that runs often, or of what the
    // library or a list makes.
    static const QRegularExpression setting(
        QStringLiteral("\\b(?:pinhole\\s*\\.\\s*setSetting\\s*\\(|pinhole\\s*\\.\\s*settings\\s*\\.\\s*\\w+\\s*=(?!=))"));
    static const QRegularExpression often(QStringLiteral("^on(Triggered|PositionChanged|Wheel|Moved|\\w+Changed)$"));
    static const QRegularExpression made(QStringLiteral("\\.(map|filter|concat|slice)\\s*\\(|\\bfield\\s*\\(|\\blibrary\\b|JSON\\.stringify|Date\\.now"));

    static const QRegularExpression shortcut(QStringLiteral("\\bShortcut\\s*\\{"));
    static const QRegularExpression importLine(QStringLiteral("^[ \\t]*import\\s+\"([^\"]+)\""), QRegularExpression::MultilineOption);
    static const QRegularExpression internal(QStringLiteral("^[ \\t]*import\\s+Pinhole\\.Internal\\b"), QRegularExpression::MultilineOption);
    const QString root = QDir::cleanPath(QDir(dir).absolutePath());
    // Pinhole's own, or a copy of one of them (its id kept): Pinhole.Internal
    // is theirs.
    QFile manifestFile(QDir(dir).filePath(QStringLiteral("manifest.json")));
    const bool builtinId = manifestFile.open(QIODevice::ReadOnly)
        && QJsonDocument::fromJson(manifestFile.readAll()).object().value(QStringLiteral("id")).toString().startsWith(QLatin1String("pinhole."));

    // The window's own objects and ids, which a plugin's QML can reach by
    // name, as it is loaded inside the window: what it is given is
    // `pinhole` and `theme`. One of the plugin's own of that name is its
    // own (an id, a property, a variable, a parameter).
    static const QSet<QString> windows = [] {
        QSet<QString> names = {"keymap", "viewer", "outgoing", "history", "incoming", "storage", "sources", "updater",
                               "plugins", "pluginGuide", "display", "stage", "markChoices", "timeline", "translator",
                               "service", "autoTours", "bench", "libraryGiven", "silent", "pluginLayer", "pluginLoader"};
        QFile main(QStringLiteral(":/Main.qml"));
        static const QRegularExpression id(QStringLiteral("^\\s*id\\s*:\\s*(\\w+)"), QRegularExpression::MultilineOption);
        if (main.open(QIODevice::ReadOnly))
            for (auto m = id.globalMatch(QString::fromUtf8(main.readAll())); m.hasNext();)
                names << m.next().captured(1);
        return names;
    }();
    QString everything;
    {
        QDirIterator all(dir, {"*.qml", "*.js"}, QDir::Files, QDirIterator::Subdirectories);
        while (all.hasNext()) {
            QFile f(all.next());
            if (f.open(QIODevice::ReadOnly))
                everything += codeOnly(QString::fromUtf8(f.readAll())) + QLatin1Char('\n');
        }
    }
    const auto declared = [&everything](const QString &name) {
        const QString n = QRegularExpression::escape(name);
        const QRegularExpression declaration(QStringLiteral(
            "\\b(?:const|let|var|function)\\s+%1\\b|\\bproperty\\s+[\\w<>.]+\\s+%1\\b|\\bid\\s*:\\s*%1\\b"
            "|\\(\\s*(?:[\\w$]+\\s*,\\s*)*%1\\s*[,)]|(?<![\\w$.])%1\\s*=>|\\bfor\\s*\\(\\s*(?:const|let|var)?\\s*%1\\b"
            "|\\bas\\s+%1\\b").arg(n));
        return declaration.match(everything).hasMatch();
    };
    static const QRegularExpression bareName(QStringLiteral("(?<![\\w$.])([A-Za-z_$][\\w$]*)\\s*\\.\\s*[A-Za-z_$]"));
    static const QRegularExpression bareImport(QStringLiteral("^[ \\t]*import\\s+Pinhole\\s*(?:$|[;\\d])"), QRegularExpression::MultilineOption);
    QHash<QString, bool> own;

    QSet<QString> strings;
    QDirIterator it(dir, {"*.qml", "*.js"}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        const QString file = QDir(dir).relativeFilePath(path);
        // Its own tests and checks are not the plugin.
        if (file.startsWith(QLatin1String("check/")) || file.startsWith(QLatin1String("tests/"))
            || file.contains(QLatin1String("target/")))
            continue;
        // The source of one of Pinhole's own plugins' native modules: its QML
        // is compiled into the module, Pinhole's code as the window's is.
        if (builtinId && file.startsWith(QLatin1String("module/")))
            continue;
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly))
            continue;
        const QString source = QString::fromUtf8(f.readAll());
        const QString code = codeOnly(source);
        const QString text = withoutComments(source);
        // Its strings but the words it shows.
        const QString literals = withoutShownWords(text);
        for (const Rule &rule : rules)
            for (auto m = rule.pattern.globalMatch(rule.inStrings ? literals : code); m.hasNext();) {
                const auto match = m.next();
                QString what = rule.what;
                if (what.contains(QLatin1String("%1")))
                    what = what.arg(match.captured(1));
                found.errors << QStringLiteral("%1:%2: %3")
                                    .arg(file)
                                    .arg(lineAt(rule.inStrings ? literals : code, match.capturedStart()))
                                    .arg(what);
            }
        // A Shortcut takes its keys over every screen, while the plugin is
        // away too, Escape and all.
        for (auto m = shortcut.globalMatch(code); m.hasNext();)
            found.warnings << QStringLiteral("%1:%2: a Shortcut takes its keys everywhere, while the plugin is not shown "
                                             "too: use a Command with keyLayer \"plugin\" and a when")
                                  .arg(file)
                                  .arg(lineAt(code, m.next().capturedStart()));
        // A file from outside the plugin: another plugin's changes, or goes,
        // with it.
        for (auto m = importLine.globalMatch(text); m.hasNext();) {
            const auto match = m.next();
            const QString imported = QDir::cleanPath(QFileInfo(path).absoluteDir().absoluteFilePath(match.captured(1)));
            if (!imported.startsWith(root + QLatin1Char('/')) && imported != root)
                found.errors << QStringLiteral("%1:%2: imports %3 from outside the plugin: another plugin's file changes, "
                                               "or is replaced, without it. Copy what it needs in")
                                    .arg(file)
                                    .arg(lineAt(text, match.capturedStart()))
                                    .arg(match.captured(1));
        }
        if (!builtinId)
            for (auto m = internal.globalMatch(code); m.hasNext();)
                found.errors << QStringLiteral("%1:%2: Pinhole.Internal is Pinhole's own plugins', and may change with any "
                                               "version: use what Pinhole.Plugin gives")
                                    .arg(file)
                                    .arg(lineAt(code, m.next().capturedStart()));
        // The window's own, reached by name: not given, and not promised.
        for (auto m = bareName.globalMatch(code); m.hasNext();) {
            const auto match = m.next();
            const QString name = match.captured(1);
            if (!windows.contains(name))
                continue;
            if (!own.contains(name))
                own.insert(name, declared(name));
            if (own.value(name))
                continue;
            own.insert(name, true);
            found.errors << QStringLiteral("%1:%2: %3 is the window's own, reached as the plugin is loaded inside it: not "
                                           "given to plugins, and it changes without notice. Use what pinhole gives "
                                           "(docs/plugins.md)")
                                .arg(file)
                                .arg(lineAt(code, match.capturedStart()))
                                .arg(name);
        }
        for (auto m = bareImport.globalMatch(code); m.hasNext();)
            found.errors << QStringLiteral("%1:%2: import Pinhole is the window's own module: import Pinhole.Plugin, "
                                           "which has what a plugin is given")
                                .arg(file)
                                .arg(lineAt(code, m.next().capturedStart()));
        // What Pinhole gives, by the names it has.
        const auto unknown = [&](const QRegularExpression &use, const QSet<QString> &names, const QString &whose) {
            QSet<QString> said;
            for (auto m = use.globalMatch(code); m.hasNext();) {
                const auto match = m.next();
                const QString name = match.captured(1);
                if (names.contains(name) || said.contains(name))
                    continue;
                said << name;
                found.errors << QStringLiteral("%1:%2: %3 has no %4%5: it is undefined as the plugin runs")
                                    .arg(file)
                                    .arg(lineAt(code, match.capturedStart()))
                                    .arg(whose, name, didYouMean(name, QStringList(names.begin(), names.end())));
            }
        };
        unknown(hostUse, hostMembers, QStringLiteral("pinhole"));
        unknown(libraryUse, libraryMembers, QStringLiteral("pinhole.library"));
        unknown(viewerUse, viewerMembers, QStringLiteral("pinhole.viewer"));
        if (!ownTheme.match(code).hasMatch())
            unknown(themeUse, themeMembers, QStringLiteral("theme"));
        for (auto m = fieldUse.globalMatch(text); m.hasNext();) {
            const auto match = m.next();
            if (!PluginLibrary::fieldNames().contains(match.captured(1)))
                found.errors << QStringLiteral("%1:%2: library.field has no \"%3\"%4: it gives an empty list. It has %5")
                                    .arg(file)
                                    .arg(lineAt(text, match.capturedStart()))
                                    .arg(match.captured(1), didYouMean(match.captured(1), PluginLibrary::fieldNames()),
                                         PluginLibrary::fieldNames().join(QStringLiteral(", ")));
        }
        // Menus over the window.
        if (controls.match(code).hasMatch())
            for (auto m = popup.globalMatch(code); m.hasNext();) {
                const auto match = m.next();
                found.errors << QStringLiteral("%1:%2: a %3 opens over the whole window, outside the plugin's Panel, and "
                                               "hides what it acts on: put actions on controls one can see (ShellButton, "
                                               "Choices, a PluginAction)")
                                    .arg(file)
                                    .arg(lineAt(code, match.capturedStart()))
                                    .arg(match.captured(1));
            }
        for (auto m = contextMenu.globalMatch(code); m.hasNext();)
            found.errors << QStringLiteral("%1:%2: a context menu hides its actions behind a right click, which a touchpad's "
                                           "two-finger tap is in Pinhole: put them on controls one can see (ShellButton, "
                                           "Choices, a PluginAction)")
                                .arg(file)
                                .arg(lineAt(code, m.next().capturedStart()));
        for (auto m = fieldCopy.globalMatch(code); m.hasNext();)
            found.errors << QStringLiteral("%1:%2: copy() sets the session's clipboard, a self-test's too: use "
                                           "pinhole.copyText")
                                .arg(file)
                                .arg(lineAt(code, m.next().capturedStart()));
        // Files by their path: the plugin's own.
        for (auto m = fileUse.globalMatch(text); m.hasNext();) {
            const auto match = m.next();
            const QString named = match.captured(1);
            // A URL (qrc:, image://, https:): not a file of a folder.
            if (named.contains(QLatin1Char(':')) && !named.startsWith(QLatin1String("file:")))
                continue;
            const QString local = named.startsWith(QLatin1String("file:")) ? QUrl(named).toLocalFile() : named;
            const QString resolved = QDir::cleanPath(QFileInfo(path).absoluteDir().absoluteFilePath(local));
            if (!resolved.startsWith(root + QLatin1Char('/')))
                found.errors << QStringLiteral("%1:%2: %3 is outside the plugin: another plugin's file changes, or is "
                                               "replaced, without it. Copy what it needs in")
                                    .arg(file)
                                    .arg(lineAt(text, match.capturedStart()))
                                    .arg(named);
        }
        // A Command's keys: the plugin's while it covers the window, or on
        // the grid or the viewer when said so, never everywhere.
        // (Found in its code, where a "[" in a string is no bracket; its
        // value read on that line of the text.)
        static const QRegularExpression layerName(QStringLiteral("(?:^|[;\\s])keyLayer\\s*:"));
        const QStringList textLines = text.split(QLatin1Char('\n'));
        for (auto m = command.globalMatch(code); m.hasNext();) {
            const auto match = m.next();
            const qsizetype open = match.capturedEnd() - 1, end = closing(code, open);
            qsizetype at = -1;
            int depth = 0;
            for (qsizetype k = open; k <= end && at < 0; ++k) {
                const QChar c = code[k];
                if (c == '{' || c == '(' || c == '[')
                    ++depth;
                else if (c == '}' || c == ')' || c == ']')
                    --depth;
                else if (depth == 1 && code.mid(k, 8) == QLatin1String("keyLayer")
                         && layerName.match(code.mid(k - 1, 40)).hasMatch())
                    at = k;
            }
            const auto layer = at < 0 ? QRegularExpressionMatch()
                                      : keyLayer.match(textLines.value(lineAt(code, at) - 1));
            const QString value = layer.captured(1).trimmed();
            if (at < 0)
                found.errors << QStringLiteral("%1:%2: a Command without a keyLayer has its keys over the grid and the "
                                               "viewer whenever the plugin is on, Pinhole's own among them: give it "
                                               "keyLayer: \"plugin\" and a when (or \"grid\" or \"viewer\")")
                                    .arg(file)
                                    .arg(lineAt(code, match.capturedStart()));
            else if (value == QLatin1String("\"any\"") || value == QLatin1String("\"chrome\"")
                     || value == QLatin1String("'any'") || value == QLatin1String("'chrome'"))
                found.errors << QStringLiteral("%1:%2: keyLayer %3 takes the keys over Pinhole's own: use \"plugin\" "
                                               "with a when, or \"grid\" or \"viewer\"")
                                    .arg(file)
                                    .arg(lineAt(code, match.capturedStart()))
                                    .arg(value);
        }
        // What a PluginMark and a PluginAction take as words: one Pinhole
        // knows, or nothing is drawn (said only as it runs).
        static const QRegularExpression markOrAction(QStringLiteral("\\b(PluginMark|PluginAction)\\s*\\{"));
        static const QRegularExpression valueOf(QStringLiteral("(?:^|[;{\\s])(icon|colour|textColour|place)\\s*:\\s*[\"']([^\"']*)[\"']"));
        static const QStringList colours = {"red", "green", "yellow", "blue", "magenta", "cyan", "accent"};
        static const QStringList icons = {"video", "live", "heart", "stack"};
        static const QStringList places = {"grid", "viewer", "selection"};
        static const QRegularExpression hex(QStringLiteral("^#[0-9A-Fa-f]{6}$"));
        for (auto m = markOrAction.globalMatch(code); m.hasNext();) {
            const auto match = m.next();
            const qsizetype open = match.capturedEnd() - 1;
            const int first = lineAt(code, open), last = lineAt(code, closing(code, open));
            const QString lines = textLines.mid(first - 1, last - first + 1).join(QLatin1Char('\n'));
            for (auto v = valueOf.globalMatch(lines); v.hasNext();) {
                const auto value = v.next();
                const QString key = value.captured(1), word = value.captured(2);
                const int line = first + int(lines.left(value.capturedStart(2)).count(QLatin1Char('\n')));
                QString wrong;
                if (key == QLatin1String("place") && match.captured(1) == QLatin1String("PluginAction") && !places.contains(word))
                    wrong = QStringLiteral("place \"%1\" is none of Pinhole's%2, and the action never shows: %3")
                                .arg(word, didYouMean(word, places), places.join(QStringLiteral(", ")));
                else if ((key == QLatin1String("colour") || key == QLatin1String("textColour")) && !word.isEmpty()
                         && !colours.contains(word) && !hex.match(word).hasMatch())
                    wrong = QStringLiteral("%1 \"%2\" is no colour of the theme's%3: %4, or \"#rrggbb\"")
                                .arg(key, word, didYouMean(word, colours), colours.join(QStringLiteral(", ")));
                else if (key == QLatin1String("icon") && match.captured(1) == QLatin1String("PluginMark") && !icons.contains(word)
                         && word.toUcs4().size() != 1 && !word.startsWith(QLatin1String("\\u")))
                    wrong = QStringLiteral("icon \"%1\" is neither %2 nor one character (a Nerd Font glyph, as \"\\uf186\")")
                                .arg(word, icons.join(QStringLiteral(", ")));
                if (!wrong.isEmpty())
                    found.errors << QStringLiteral("%1:%2: ").arg(file).arg(line) + wrong;
            }
        }
        for (auto m = joined.globalMatch(text); m.hasNext();)
            found.warnings << QStringLiteral("%1:%2: ").arg(file).arg(lineAt(text, m.next().capturedStart()))
                    + QStringLiteral("words put together inside qsTr are never translated: one literal, with %1 and "
                                     ".arg(), or %n for a count");
        for (auto m = picturesPath.globalMatch(literals); m.hasNext();) {
            const auto match = m.next();
            found.errors << QStringLiteral("%1:%2: %3 by hand is the real Pictures in a self-test: use pinhole.picturePath, "
                                           "or pinhole.picturesDir")
                                .arg(file)
                                .arg(lineAt(literals, match.capturedStart()))
                                .arg(match.captured(1));
        }
        if (multimedia.match(code).hasMatch())
            for (auto m = video.globalMatch(code); m.hasNext();)
                found.errors << QStringLiteral("%1:%2: a Video shows an HDR video (an iPhone's) flat and grey, or its "
                                               "brightest colours broken: use a MediaPlayer and a VideoView")
                                    .arg(file)
                                    .arg(lineAt(code, m.next().capturedStart()));
        // A MouseArea filling the plugin's root, or what fills it.
        for (auto m = mouseArea.globalMatch(code); m.hasNext();) {
            const auto match = m.next();
            if (!fills.match(ownLines(code, match.capturedEnd() - 1)).hasMatch())
                continue;
            auto [type, open] = enclosingObject(code, match.capturedStart());
            while ((type == QLatin1String("Item") || type == QLatin1String("Rectangle")) && fills.match(ownLines(code, open)).hasMatch())
                std::tie(type, open) = enclosingObject(code, open);
            if (type == QLatin1String("PinholePlugin"))
                found.errors << QStringLiteral("%1:%2: a MouseArea over the whole window lets the wheel and a touchpad's "
                                               "pinch through to the photos: put what the plugin shows in a Panel, which "
                                               "takes them")
                                    .arg(file)
                                    .arg(lineAt(code, match.capturedStart()));
        }
        for (auto m = said.globalMatch(text); m.hasNext();) {
            const auto match = m.next();
            // Single quotes' words as JSON's double-quoted.
            const QString words = match.capturedStart(1) >= 0
                ? match.captured(1)
                : QString(match.captured(2)).replace(QLatin1String("\\'"), QLatin1String("'")).replace(QLatin1String("\""), QLatin1String("\\\""));
            strings << QJsonDocument::fromJson("[\"" + words.toUtf8() + "\"]").array().at(0).toString();
            if (const auto key = rawKey.match(words); key.hasMatch())
                found.warnings << QStringLiteral("%1:%2: \"%3\" names a key as Qt writes it: say it as the buttons do "
                                                 "(⏎, Esc, ⇧ S, Ctrl Z, ← →)")
                                      .arg(file)
                                      .arg(lineAt(text, match.capturedStart()))
                                      .arg(key.captured(0));
        }
        // The help's words, each entry's second.
        for (auto m = helpList.globalMatch(text); m.hasNext();) {
            const qsizetype open = m.next().capturedEnd() - 1;
            const QString list = text.mid(open, closing(text, open) - open + 1);
            for (auto e = helpEntry.globalMatch(list); e.hasNext();) {
                const auto entry = e.next();
                const QString words = entry.captured(1);
                if (words.contains(wordy) && !brand.match(words).hasMatch())
                    found.warnings << QStringLiteral("%1:%2: \"%3\" is shown as it is, in every language: qsTr(\"%3\")")
                                          .arg(file)
                                          .arg(lineAt(text, open + entry.capturedStart(1)))
                                          .arg(words);
            }
        }
        for (auto m = shown.globalMatch(text); m.hasNext();) {
            const auto match = m.next();
            const QString words = match.captured(2).isEmpty() ? match.captured(3) : match.captured(2);
            // A name as it is everywhere (iCloud, iPhone) is no word to translate.
            if (words.contains(wordy) && !brand.match(words).hasMatch())
                found.warnings << QStringLiteral("%1:%2: \"%3\" is shown as it is, in every language: qsTr(\"%3\")")
                                      .arg(file)
                                      .arg(lineAt(text, match.capturedStart()))
                                      .arg(words);
        }
        for (auto m = setting.globalMatch(code); m.hasNext();) {
            const auto match = m.next();
            const QString handler = handlerAround(code, match.capturedStart());
            const qsizetype end = code.indexOf('\n', match.capturedEnd());
            const QString rest = code.mid(match.capturedEnd(), end < 0 ? -1 : end - match.capturedEnd());
            const bool frequent = often.match(handler).hasMatch();
            if (frequent || rest.contains(made))
                found.warnings << QStringLiteral("%1:%2: a setting written %3: settings are the user's choices, and "
                                                 "plugins.json is written again each time; keep what changes often or "
                                                 "grows in pinhole.state, what can be worked out again in pinhole.cache")
                                      .arg(file)
                                      .arg(lineAt(code, match.capturedStart()))
                                      .arg(frequent ? QStringLiteral("in %1, which runs often").arg(handler)
                                                    : QStringLiteral("from a list or the library"));
        }
    }
    // Its words in Polish, as Pinhole's tests ask of Pinhole's own: its own
    // catalog's, or Pinhole's.
    if (!strings.isEmpty()) {
        const auto catalog = [](const QString &path) {
            QFile f(path);
            return f.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(f.readAll()).object() : QJsonObject();
        };
        const QJsonObject own = catalog(dir + "/i18n/pl.json"), pinholes = catalog(QStringLiteral(":/i18n/pl.json"));
        const auto notPinholes = std::count_if(strings.begin(), strings.end(), [&](const QString &s) { return !pinholes.contains(s); });
        if (!QFileInfo::exists(dir + "/i18n/pl.json")) {
            if (notPinholes > 0)
                found.warnings << QStringLiteral("i18n/pl.json: there is none, and %1 of its words are not Pinhole's own: "
                                                 "they show in English in Polish")
                                      .arg(notPinholes);
        } else {
            QStringList missing;
            for (const QString &english : strings)
                if (!own.contains(english) && !pinholes.contains(english))
                    missing << english;
            missing.sort();
            for (const QString &english : missing)
                found.warnings << QStringLiteral("i18n/pl.json: no Polish for \"%1\"").arg(english);
        }
    }
    return found;
}

QStringList Plugins::nameClashes(const QString &dir, const QList<std::pair<QString, QString>> &others) {
    // Its files by name, as a qsTr() names them (Translator).
    auto filesIn = [](const QString &folder, QDirIterator::IteratorFlags flags) {
        QMap<QString, QString> files;
        QDirIterator it(folder, {"*.qml", "*.js"}, QDir::Files, flags);
        while (it.hasNext()) {
            const QFileInfo info(it.next());
            if (!info.filePath().contains(QLatin1String("/target/")))
                files.insert(info.completeBaseName(), info.fileName());
        }
        return files;
    };
    const QMap<QString, QString> own = filesIn(dir, QDirIterator::Subdirectories);
    const QMap<QString, QString> pinholes = filesIn(QStringLiteral(":/"), QDirIterator::NoIteratorFlags);
    const QString self = QDir(dir).canonicalPath();
    QStringList clashes;
    for (auto file = own.cbegin(); file != own.cend(); ++file) {
        // A main file is told apart by its URL.
        if (file.key() == QLatin1String("Main"))
            continue;
        QStringList whose;
        if (pinholes.contains(file.key()))
            whose << QStringLiteral("Pinhole's own %1").arg(pinholes.value(file.key()));
        for (const auto &[id, folder] : others) {
            if (QDir(folder).canonicalPath() == self)
                continue;
            if (const QMap<QString, QString> theirs = filesIn(folder, QDirIterator::Subdirectories); theirs.contains(file.key()))
                whose << QStringLiteral("%1's %2").arg(id, theirs.value(file.key()));
        }
        if (!whose.isEmpty())
            clashes << QStringLiteral("%1 is named as %2").arg(file.value(), whose.join(QStringLiteral(", ")));
    }
    return clashes;
}

bool Plugins::usableId(const QString &id) {
    // What its folders and files are named after (state, cache, logs);
    // pinholed's usable_id is the same.
    static const QRegularExpression grammar(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$"));
    return grammar.match(id).hasMatch() && !id.contains(QLatin1String(".."));
}

QStringList Plugins::manifestWarnings(const QString &dir, const QJsonObject &manifest) {
    QStringList said;
    // What a manifest has: a key misspelt is never read, and nothing says so.
    static const QStringList keys = {"schemaVersion", "id", "name", "description", "version", "api", "entryPoints",
                                     "sources", "workers", "author", "license"};
    for (auto it = manifest.begin(); it != manifest.end(); ++it)
        if (!keys.contains(it.key()))
            said << QStringLiteral("manifest.json: \"%1\" is not a manifest's key%2, and is never read")
                        .arg(it.key(), didYouMean(it.key(), keys));
    for (const QString number : {"api", "schemaVersion"})
        if (manifest.contains(number)) {
            const QJsonValue value = manifest.value(number);
            if (!value.isDouble() || value.toDouble() != double(value.toInt()))
                said << QStringLiteral("manifest.json: \"%1\" is a whole number (\"%1\": %2), not %3")
                            .arg(number)
                            .arg(number == QLatin1String("api") ? api : 1)
                            .arg(QString::fromUtf8(QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact)).mid(1).chopped(1));
        }
    // pinhole. ids are Pinhole's own: one of one's own takes such an id
    // only as a copy of Pinhole's plugin of it.
    const QString id = manifest.value("id").toString();
    if (id.startsWith(QLatin1String("pinhole.")) && !QFileInfo::exists(builtInDir() + '/' + id.mid(8) + "/manifest.json")
        && !QDir(dir).absolutePath().startsWith(builtInDir() + '/'))
        said << QStringLiteral("manifest.json: \"pinhole.\" ids are Pinhole's own plugins': give it one of yours "
                               "(me.%1)").arg(id.mid(8));
    return said;
}

QString Plugins::manifestProblem(const QString &dir, QJsonObject *manifest, QString *main) {
    QFile file(dir + "/manifest.json");
    if (!file.open(QIODevice::ReadOnly))
        return tr("There is no manifest.json in %1").arg(dir);
    QJsonParseError parseError;
    *manifest = QJsonDocument::fromJson(file.readAll(), &parseError).object();
    const QString id = manifest->value("id").toString(QFileInfo(dir).fileName());
    *main = manifest->value("entryPoints").toObject().value("main").toString();
    if (parseError.error != QJsonParseError::NoError)
        return tr("manifest.json is not JSON: %1").arg(parseError.errorString());
    if (manifest->value("schemaVersion").toInt() != 1)
        return tr("manifest.json needs \"schemaVersion\": 1");
    if (!usableId(id))
        return tr("manifest.json has no usable \"id\": letters, digits, \".\", \"_\" and \"-\", beginning with a letter or a digit (me.night)");
    if (!safeEntry(*main) || !QFileInfo::exists(dir + '/' + *main))
        return tr("manifest.json's \"entryPoints\" has no \"main\" file in the plugin's folder");
    if (const int wanted = manifest->value("api").toInt(1); wanted > api)
        return tr("It needs a newer Pinhole: plugin API %1, and this one has %2.").arg(wanted).arg(api);
    if (const QString problem = sourcesProblem(dir, id, *manifest); !problem.isEmpty())
        return problem;
    return workersProblem(dir, *manifest);
}

QString Plugins::workersProblem(const QString &dir, const QJsonObject &manifest) {
    // As pinholed takes them (core/src/plugins.rs, docs/plugins.md, Workers),
    // so what it would refuse is said here.
    const QJsonValue declared = manifest.value("workers");
    if (declared.isUndefined())
        return {};
    if (!declared.isArray())
        return tr("manifest.json's \"workers\" is a list");
    if (manifest.value("api").toInt(1) < 11)
        return tr("workers need \"api\": 11");
    static const QRegularExpression named(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$"));
    QStringList seen;
    for (const QJsonValue &each : declared.toArray()) {
        const QJsonObject worker = each.toObject();
        const QString name = worker.value("name").toString();
        if (!each.isObject() || !named.match(name).hasMatch())
            return tr("each of manifest.json's \"workers\" has a \"name\" of letters, digits, '.', '_' and '-'");
        if (seen.contains(name))
            return tr("\"workers\": %1 is named twice").arg(name);
        seen << name;
        const auto wrong = [&name](const QString &why) { return tr("\"workers\": %1: %2").arg(name, why); };
        if (worker.contains("per") && worker.value("per") != QJsonValue(QStringLiteral("source")))
            return wrong(tr("\"per\" is \"source\""));
        if (worker.contains("args")) {
            const QJsonValue args = worker.value("args");
            bool fine = args.isArray() && args.toArray().size() <= 64;
            for (const QJsonValue &a : args.toArray())
                fine = fine && a.isString() && a.toString().toUtf8().size() <= 4096;
            if (!fine)
                return wrong(tr("\"args\" is a list of strings"));
        }
        const QString program = worker.value("program").toString();
        if (!safeEntry(program))
            return wrong(tr("\"program\" is a path in the plugin's folder, without \"..\""));
        if (const QFileInfo file(dir + '/' + program); !file.isFile() || !file.isExecutable())
            return wrong(tr("%1 is not a program in the plugin's folder (a file it can run)").arg(program));
    }
    return {};
}

QStringList Plugins::sourceKinds(const QJsonObject &manifest) {
    QStringList kinds;
    for (const QJsonValue &source : manifest.value("sources").toArray())
        if (const QString kind = source.toObject().value("kind").toString(); !kind.isEmpty())
            kinds << kind;
    return kinds;
}

QString Plugins::sourcesProblem(const QString &dir, const QString &id, const QJsonObject &manifest) {
    const QJsonValue declared = manifest.value("sources");
    if (declared.isUndefined())
        return {};
    if (!declared.isArray())
        return tr("manifest.json's \"sources\" is a list");
    // Pinhole's own plugins' folder; the kinds its plugins bring with no
    // provider (pinholed's own); and those of its plugin of this id.
    const QString builtIn = QDir(builtInDir()).canonicalPath();
    const bool own = !builtIn.isEmpty() && QDir(dir).canonicalPath().startsWith(builtIn + '/');
    QStringList ownKinds, pinholeds;
    for (const QFileInfo &each : QDir(builtInDir()).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        QFile file(each.filePath() + "/manifest.json");
        if (!file.open(QIODevice::ReadOnly))
            continue;
        const QJsonObject theirs = QJsonDocument::fromJson(file.readAll()).object();
        for (const QJsonValue &source : theirs.value("sources").toArray())
            if (!source.toObject().contains("provider"))
                pinholeds << source.toObject().value("kind").toString();
        if (theirs.value("id").toString(each.fileName()) == id)
            ownKinds = sourceKinds(theirs);
    }
    static const QRegularExpression named(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$"));
    static const QRegularExpression extension(QStringLiteral("^[a-z0-9]{1,16}$"));
    QStringList seen;
    for (const QJsonValue &each : declared.toArray()) {
        const QJsonObject source = each.toObject();
        const QString kind = source.value("kind").toString();
        if (!each.isObject() || kind.isEmpty())
            return tr("each of manifest.json's \"sources\" names its \"kind\"");
        if (seen.contains(kind))
            return tr("\"sources\": %1 is named twice").arg(kind);
        seen << kind;
        // A kind with no provider is one of pinholed's own (iCloud's,
        // folders'): only Pinhole's own plugin of it brings it, or one's own
        // copy of that plugin.
        if (!source.contains("provider")) {
            if (!own && !ownKinds.contains(kind))
                return tr("\"sources\": %1 is pinholed's own kind, which only Pinhole's own plugin of it brings").arg(kind);
            continue;
        }
        // One of its own, run by its provider: as pinholed takes it
        // (docs/source-providers.md), so what it would refuse is said here.
        const auto wrong = [&kind](const QString &why) { return tr("\"sources\": %1: %2").arg(kind, why); };
        if (manifest.value("api").toInt(1) < 3)
            return wrong(tr("a source needs \"api\": 3"));
        if (!named.match(kind).hasMatch())
            return wrong(tr("a kind is letters, digits, '.', '_' and '-'"));
        if (kind != id && !kind.startsWith(id + '.'))
            return wrong(tr("its kind must be the plugin's id, %1, or start with %1 and a dot").arg(id));
        if (pinholeds.contains(kind))
            return wrong(tr("that kind is pinholed's own"));
        const QString provider = source.value("provider").toString();
        if (!safeEntry(provider))
            return wrong(tr("\"provider\" is a path in the plugin's folder, without \"..\""));
        if (const QFileInfo program(dir + '/' + provider); !program.isFile() || !program.isExecutable())
            return wrong(tr("%1 is not a program in the plugin's folder (a file it can run)").arg(provider));
        if (source.contains("needs_sign_in") && !source.value("needs_sign_in").isBool())
            return wrong(tr("\"needs_sign_in\" is true or false"));
        if (source.contains("accepts")) {
            const QJsonValue accepts = source.value("accepts");
            bool fine = accepts.isArray();
            for (const QJsonValue &e : accepts.toArray())
                fine = fine && extension.match(e.toString()).hasMatch();
            if (!fine)
                return wrong(tr("\"accepts\" is a list of extensions in lower case, without the dot"));
        }
        if (source.contains("rounds_every_secs")) {
            const double every = source.value("rounds_every_secs").toDouble(-1);
            if (every != std::floor(every) || every < 5 || every > 86400)
                return wrong(tr("\"rounds_every_secs\" is a whole number from 5 to 86400"));
        }
    }
    return {};
}
