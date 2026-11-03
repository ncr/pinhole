#include "pluginguide.h"
#include "plugins.h"
#include "selftest.h"

#include <QFile>
#include <QMimeData>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>

int PluginGuide::api() const { return Plugins::api; }

QStringList PluginGuide::documents() {
    return {"plugin-quickstart", "plugins", "plugin-examples", "source-providers", "plugin-agents"};
}

QString PluginGuide::document(const QString &name) {
    if (!documents().contains(name)) return {};
    QFile file(QStringLiteral(":/docs/%1.md").arg(name));
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(file.readAll());
}

namespace {
QString slug(QString title) {
    title = title.toLower();
    title.remove(QRegularExpression(QStringLiteral("[^\\p{L}\\p{N} _-]")));
    title.replace(' ', '-');
    return title;
}
}

QVariantList PluginGuide::sections(const QString &name) const {
    QVariantList result;
    QVariantMap part;
    QString contents;
    QString fence;
    QSet<QString> ids;
    const auto finish = [&] {
        if (part.isEmpty()) return;
        part.insert("markdown", contents.trimmed());
        result.append(part);
        contents.clear();
    };
    // Headings inside examples belong to their code, not the navigation.
    for (const QString &line : document(name).split('\n')) {
        const QString trimmed = line.trimmed();
        if (trimmed.startsWith("```") || trimmed.startsWith("~~~")) {
            if (fence.isEmpty()) fence = trimmed.left(3);
            else if (trimmed.startsWith(fence)) fence.clear();
        }
        if (fence.isEmpty() && (line.startsWith("# ") || line.startsWith("## ") || line.startsWith("### "))) {
            finish();
            const QString title = line.mid(line.indexOf(' ') + 1).trimmed();
            const QString base = slug(title);
            QString id = base;
            for (int i = 2; ids.contains(id); ++i) id = base + '-' + QString::number(i);
            ids.insert(id);
            part = {{"id", id}, {"title", title}};
        }
        contents += line + '\n';
    }
    finish();
    return result;
}

QString PluginGuide::markdown(const QString &name, const QString &section) const {
    if (section.isEmpty()) return document(name);
    for (const QVariant &part : sections(name)) {
        const QVariantMap entry = part.toMap();
        if (entry.value("id").toString() == section) return entry.value("markdown").toString();
    }
    return {};
}

QVariantMap PluginGuide::destination(const QString &link, const QString &current) const {
    const QUrl url(link);
    if (!url.isRelative()) return {};
    QString name = url.path();
    if (name.isEmpty()) name = current;
    else {
        if (name.startsWith("./")) name.remove(0, 2);
        if (name.endsWith(".md")) name.chop(3);
    }
    if (!documents().contains(name)) return {};
    const QString target = url.fragment(QUrl::FullyDecoded);
    if (target.isEmpty()) return {{"document", name}, {"section", 0}};
    const auto parts = sections(name);
    for (int i = 0; i < parts.size(); ++i) {
        const auto part = parts[i].toMap();
        if (part.value("id") == target)
            return {{"document", name}, {"section", i}};
    }
    return {};
}

void PluginGuide::copy(const QString &text) const {
    auto *data = new QMimeData;
    data->setText(text);
    setClipboard(data);
}
