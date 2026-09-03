#include "match/TextSimilarity.h"

#include <QSet>

#include <algorithm>
#include <vector>

namespace pl::match {

int editDistance(const QString &a, const QString &b)
{
    const int n = a.size();
    const int m = b.size();
    if (n == 0)
        return m;
    if (m == 0)
        return n;

    std::vector<int> prev(m + 1);
    std::vector<int> curr(m + 1);
    for (int j = 0; j <= m; ++j)
        prev[j] = j;

    for (int i = 1; i <= n; ++i) {
        curr[0] = i;
        const QChar ca = a.at(i - 1);
        for (int j = 1; j <= m; ++j) {
            const int cost = (ca == b.at(j - 1)) ? 0 : 1;
            curr[j] = std::min({prev[j] + 1, curr[j - 1] + 1, prev[j - 1] + cost});
        }
        std::swap(prev, curr);
    }
    return prev[m];
}

double editSimilarity(const QString &a, const QString &b)
{
    if (a.isEmpty() && b.isEmpty())
        return 1.0;
    const int maxLen = std::max(a.size(), b.size());
    if (maxLen == 0)
        return 1.0;
    return 1.0 - double(editDistance(a, b)) / maxLen;
}

namespace {
QSet<QString> trigrams(const QString &s)
{
    const QString padded = QStringLiteral("  ") + s.toLower() + QLatin1Char(' ');
    QSet<QString> grams;
    for (int i = 0; i + 3 <= padded.size(); ++i)
        grams.insert(padded.mid(i, 3));
    return grams;
}
} // namespace

double trigramSimilarity(const QString &a, const QString &b)
{
    if (a.isEmpty() && b.isEmpty())
        return 1.0;
    const QSet<QString> ga = trigrams(a);
    const QSet<QString> gb = trigrams(b);
    if (ga.isEmpty() || gb.isEmpty())
        return 0.0;

    int intersection = 0;
    for (const QString &g : ga) {
        if (gb.contains(g))
            ++intersection;
    }
    const int uni = ga.size() + gb.size() - intersection;
    return uni == 0 ? 0.0 : double(intersection) / uni;
}

double nameSimilarity(const QString &a, const QString &b)
{
    return std::max(editSimilarity(a.toLower(), b.toLower()), trigramSimilarity(a, b));
}

} // namespace pl::match
