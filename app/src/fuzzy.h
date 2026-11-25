#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>

// Finding a command by a few letters, for the palette (HelpOverlay.qml): what
// was typed against what each command is called, as fzf scores it (its
// second algorithm, without its long-text limits, the strings being short):
// each letter in order, scored more at the start of a word and more again
// for letters that follow on, less for the gaps between. "kal" finds
// "Kalendarz", "ep" "Eksportuj… PNG" by the initials, "zdjecie" "Zdjęcie"
// (letters are compared without their marks, "ł" as "l"), and a word typed
// with one slip ("eksprot", "kalnedarz") the word it was meant for, by the
// edits from it to the start of a word (two for a long one), scored below
// any clean match. Words typed apart must each be found, in any order.
class Fuzzy : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;

    // Lowercase, without marks ("ż" as "z", "ł" as "l"), the keys' symbols
    // as words ("⇧" as "shift"), any dash as "-": what is compared.
    static QString fold(const QString &text);
    // How well one word typed matches a text, both folded; 0 for not at all.
    static int score(const QString &word, const QString &text);
    // What was typed (words apart) against a command's names, each [text,
    // weight in percent]: the words' best scores, each found in one of them,
    // weighted and summed; 0 if one word is in none.
    static int score(const QString &query, const QList<std::pair<QString, int>> &names);
    // Whether what was typed is one of these keys, as the buttons and the
    // help show them ("⇧ E", "Ctrl ⇧ E", "H") or as written ("Shift+E").
    static bool isKey(const QString &query, const QStringList &keys);

    // The candidates that match, best first, as their places in the list.
    // Each {names: [[text, weight], …], keys: […]}: its names weighed as
    // score() does, and one whose key is what was typed first of all. Ties
    // keep the list's order.
    Q_INVOKABLE QVariantList rank(const QString &query, const QVariantList &candidates) const;
};
