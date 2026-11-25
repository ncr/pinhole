#include "fuzzy.h"

#include <QRegularExpression>

#include <algorithm>
#include <limits>
#include <vector>

namespace {

// fzf's scores (src/algo/algo.go): a letter found, a gap begun and made
// longer, and the bonus of a letter by what is before it.
constexpr int Match = 16;
constexpr int GapStart = -3;
constexpr int GapExtension = -1;
constexpr int BoundaryWhite = Match / 2 + 2;  // after a space, or the first
constexpr int BoundaryDelimiter = Match / 2 + 1;
constexpr int Boundary = Match / 2;  // after any other non-letter
constexpr int Consecutive = -(GapStart + GapExtension);
constexpr int FirstMultiplier = 2;
constexpr int None = std::numeric_limits<int>::min() / 4;

int bonusAt(const QString &text, int at) {
    if (at == 0)
        return BoundaryWhite;
    const QChar before = text.at(at - 1), here = text.at(at);
    if (!here.isLetterOrNumber())
        return 0;
    if (before.isSpace())
        return BoundaryWhite;
    if (QStringLiteral("/,:;|-_+.").contains(before))
        return BoundaryDelimiter;
    if (!before.isLetterOrNumber())
        return Boundary;
    if (before.isLetter() != here.isLetter())
        return Boundary;
    return 0;
}

// The letters in order, best placed: Smith-Waterman as fzf's v2, the gaps
// affine (one begun costs more than one made longer).
int subsequence(const QString &word, const QString &text) {
    const int m = word.size(), n = text.size();
    if (m == 0 || m > n)
        return 0;
    // Each letter's first place it can be, to give up early.
    int from = 0;
    for (const QChar c : word) {
        from = text.indexOf(c, from);
        if (from < 0)
            return 0;
        ++from;
    }
    std::vector<int> bonus(n);
    for (int j = 0; j < n; ++j)
        bonus[j] = bonusAt(text, j);
    // The row before and this one: the best score with the word's letters
    // to i, its i-th at j, and the bonus its run of letters began with.
    std::vector<int> before(n, None), here(n, None), runBefore(n, 0), run(n, 0);
    for (int i = 0; i < m; ++i) {
        std::fill(here.begin(), here.end(), None);
        // The best of the row before at least two back, less its gap.
        int gapped = None;
        for (int j = 0; j < n; ++j) {
            if (i > 0 && j >= 2 && before[j - 2] > None)
                gapped = std::max(gapped > None ? gapped + GapExtension : None, before[j - 2] + GapStart);
            else if (gapped > None)
                gapped += GapExtension;
            if (text.at(j) != word.at(i))
                continue;
            if (i == 0) {
                here[j] = Match + bonus[j] * FirstMultiplier;
                run[j] = bonus[j];
                continue;
            }
            int best = None, bestRun = 0;
            if (j >= 1 && before[j - 1] > None) {
                int first = runBefore[j - 1];
                int b = bonus[j];
                if (b >= Boundary && b > first)
                    first = b;
                b = std::max({b, first, Consecutive});
                best = before[j - 1] + Match + b;
                bestRun = first;
            }
            if (gapped > None && gapped + Match + bonus[j] > best) {
                best = gapped + Match + bonus[j];
                bestRun = bonus[j];
            }
            here[j] = best;
            run[j] = bestRun;
        }
        std::swap(before, here);
        std::swap(runBefore, run);
    }
    const int best = *std::max_element(before.begin(), before.end());
    return best > 0 ? best : (best > None ? 1 : 0);
}

// The fewest edits (a letter in, out, changed, or two swapped) from the
// word to the start of `into`, no shorter than the word less `most`.
int editsToStart(const QString &word, const QString &into, int most) {
    const int m = word.size(), n = into.size();
    std::vector<std::vector<int>> d(m + 1, std::vector<int>(n + 1));
    for (int i = 0; i <= m; ++i)
        d[i][0] = i;
    for (int j = 0; j <= n; ++j)
        d[0][j] = j;
    for (int i = 1; i <= m; ++i)
        for (int j = 1; j <= n; ++j) {
            const int changed = word.at(i - 1) == into.at(j - 1) ? 0 : 1;
            d[i][j] = std::min({d[i - 1][j] + 1, d[i][j - 1] + 1, d[i - 1][j - 1] + changed});
            if (i > 1 && j > 1 && word.at(i - 1) == into.at(j - 2) && word.at(i - 2) == into.at(j - 1))
                d[i][j] = std::min(d[i][j], d[i - 2][j - 2] + 1);
        }
    int best = std::numeric_limits<int>::max();
    for (int j = std::max(1, m - most); j <= n; ++j)
        best = std::min(best, d[m][j]);
    return best;
}

// A word typed with a slip: one edit from the start of one of the text's
// words for four letters or more, two for eight.
int slipped(const QString &word, const QString &text) {
    const int m = word.size();
    const int most = m >= 8 ? 2 : m >= 4 ? 1 : 0;
    if (most == 0)
        return 0;
    static const QRegularExpression apart(QStringLiteral("[^\\p{L}\\p{N}]+"));
    int best = 0;
    for (const QString &into : text.split(apart, Qt::SkipEmptyParts)) {
        if (into.size() < m - most)
            continue;
        const int edits = editsToStart(word, into, most);
        // As the word typed right at a word's start would score, less
        // two letters' worth for each edit: below that, above its
        // letters strewn through other words.
        const int clean = Match * m + BoundaryWhite * FirstMultiplier + (m - 1) * BoundaryWhite + Match / 2 * m;
        if (edits <= most)
            best = std::max(best, clean - (2 * Match + Boundary) * edits);
    }
    return std::max(0, best);
}

QString keyFolded(const QString &key) {
    QString folded = Fuzzy::fold(key);
    folded.remove(QRegularExpression(QStringLiteral("[\\s+]")));
    return folded;
}

// The word whole at the start of one of the text's: what someone typing a
// name means far more often than its letters strewn through another
// (Raycast and VS Code rank these first too).
int wordStart(const QString &word, const QString &text) {
    for (int at = text.indexOf(word); at >= 0; at = text.indexOf(word, at + 1))
        if (bonusAt(text, at) >= Boundary)
            return Match / 2 * word.size();
    return 0;
}

} // namespace

QString Fuzzy::fold(const QString &text) {
    QString out;
    out.reserve(text.size());
    for (const QChar c : text.normalized(QString::NormalizationForm_KD)) {
        if (c.category() == QChar::Mark_NonSpacing || c.category() == QChar::Mark_SpacingCombining
            || c.category() == QChar::Mark_Enclosing)
            continue;
        switch (c.unicode()) {
        case u'ł': case u'Ł': out += u'l'; break;
        case u'ß': out += QStringLiteral("ss"); break;
        case u'⇧': out += QStringLiteral("shift "); break;
        case u'⏎': out += QStringLiteral("enter"); break;
        case u'–': case u'—': case u'−': out += u'-'; break;
        case u'„': case u'”': case u'“': out += u'"'; break;
        default: out += c.toLower();
        }
    }
    return out;
}

int Fuzzy::score(const QString &word, const QString &text) {
    if (word.isEmpty())
        return 0;
    const int letters = subsequence(word, text);
    return std::max(letters + (letters > 0 ? wordStart(word, text) : 0), slipped(word, text));
}

int Fuzzy::score(const QString &query, const QList<std::pair<QString, int>> &names) {
    const QStringList words = fold(query).split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    if (words.isEmpty())
        return 0;
    QList<std::pair<QString, int>> folded;
    for (const auto &[text, weight] : names)
        if (!text.isEmpty())
            folded.append({fold(text), weight});
    int total = 0;
    for (const QString &word : words) {
        int best = 0;
        for (const auto &[text, weight] : folded)
            best = std::max(best, score(word, text) * weight / 100);
        if (best == 0)
            return 0;
        total += best;
    }
    return total;
}

bool Fuzzy::isKey(const QString &query, const QStringList &keys) {
    const QString typed = keyFolded(query);
    if (typed.isEmpty())
        return false;
    return std::any_of(keys.cbegin(), keys.cend(), [&](const QString &key) { return keyFolded(key) == typed; });
}

QVariantList Fuzzy::rank(const QString &query, const QVariantList &candidates) const {
    struct Found { int score, at; };
    std::vector<Found> found;
    for (int i = 0; i < candidates.size(); ++i) {
        const QVariantMap candidate = candidates.at(i).toMap();
        QList<std::pair<QString, int>> names;
        for (const QVariant &name : candidate.value(QStringLiteral("names")).toList()) {
            const QVariantList pair = name.toList();
            if (pair.size() == 2)
                names.append({pair.at(0).toString(), pair.at(1).toInt()});
        }
        // Its key, typed, before any name: "h" is H's command first.
        const int byKey = isKey(query, candidate.value(QStringLiteral("keys")).toStringList()) ? 100000 : 0;
        const int s = byKey + score(query, names);
        if (s > 0)
            found.push_back({s, i});
    }
    std::stable_sort(found.begin(), found.end(), [](const Found &a, const Found &b) { return a.score > b.score; });
    QVariantList order;
    for (const Found &f : found)
        order << f.at;
    return order;
}
