#include "match/TaxonResolver.h"

#include "match/TextSimilarity.h"
#include "taxonomy/TaxonomyStore.h"

#include <QHash>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QVariant>

#include <algorithm>

namespace pl::match {
namespace {

using taxonomy::TaxonomyStore;

double baseScoreFor(const QString &kind)
{
    if (kind == QLatin1String("accepted"))
        return 1.00;
    if (kind == QLatin1String("misspelling"))
        return 0.92;
    if (kind == QLatin1String("synonym"))
        return 0.95;
    if (kind == QLatin1String("vernacular"))
        return 0.82;
    return 0.70;
}

QString genusOf(const QString &scientificName)
{
    return scientificName.section(QLatin1Char(' '), 0, 0);
}

} // namespace

TaxonResolver::TaxonResolver(QString connectionName)
    : m_connectionName(std::move(connectionName))
{
}

bool TaxonResolver::isMarkedNonTaxon(const QString &rawText, const QString &folderPath) const
{
    QSqlQuery q(QSqlDatabase::database(m_connectionName, false));
    q.prepare(QStringLiteral(
        "SELECT 1 FROM name_alias WHERE raw_folded = ? AND not_a_taxon = 1 "
        "AND (scope = 'global' OR scope = ?) LIMIT 1"));
    q.addBindValue(TaxonomyStore::foldName(rawText));
    q.addBindValue(folderPath.isEmpty() ? QStringLiteral("global") : folderPath);
    return q.exec() && q.next();
}

QList<TaxonCandidate> TaxonResolver::exactCandidates(const ParsedName &parsed) const
{
    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    QList<TaxonCandidate> out;

    auto lookup = [&](const QString &folded, const QString &restrictRank) {
        QString sql = QStringLiteral(
            "SELECT tn.kind, t.id, t.inat_id, t.name, t.rank, t.common_name "
            "FROM taxon_name tn JOIN taxon t ON t.id = tn.taxon_id "
            "WHERE tn.name_folded = ?");
        if (!restrictRank.isEmpty())
            sql += QStringLiteral(" AND t.rank = ?");

        QSqlQuery q(db);
        q.prepare(sql);
        q.addBindValue(folded);
        if (!restrictRank.isEmpty())
            q.addBindValue(restrictRank);
        if (!q.exec())
            return;
        while (q.next()) {
            TaxonCandidate c;
            c.matchedVia = q.value(0).toString();
            c.taxonId = q.value(1).toLongLong();
            c.inatId = q.value(2).toLongLong();
            c.name = q.value(3).toString();
            c.rank = q.value(4).toString();
            c.commonName = q.value(5).toString();
            c.score = baseScoreFor(c.matchedVia);
            out.append(c);
        }
    };

    // Alias table first — an exact user decision beats everything.
    {
        QSqlQuery q(db);
        q.prepare(QStringLiteral(
            "SELECT t.id, t.inat_id, t.name, t.rank, t.common_name "
            "FROM name_alias a JOIN taxon t ON t.id = a.taxon_id "
            "WHERE a.raw_folded = ? AND a.not_a_taxon = 0 "
            "ORDER BY (a.scope <> 'global') DESC LIMIT 1"));
        q.addBindValue(TaxonomyStore::foldName(parsed.raw));
        if (q.exec() && q.next()) {
            TaxonCandidate c;
            c.taxonId = q.value(0).toLongLong();
            c.inatId = q.value(1).toLongLong();
            c.name = q.value(2).toString();
            c.rank = q.value(3).toString();
            c.commonName = q.value(4).toString();
            c.matchedVia = QStringLiteral("alias");
            c.score = 1.0;
            out.append(c);
            return out;
        }
    }

    const QString binomial = parsed.foldedBinomial();
    if (!binomial.isEmpty()) {
        lookup(binomial, {});
        if (!parsed.infraEpithet.isEmpty()) {
            lookup(QStringLiteral("%1 %2").arg(binomial, parsed.infraEpithet), {});
        }
    }

    // The whole raw string, folded — catches common names ("Pacific Black Duck")
    // and any spelling stored verbatim in taxon_name.
    const QString rawFolded = TaxonomyStore::foldName(parsed.raw);
    if (!rawFolded.isEmpty() && rawFolded != binomial)
        lookup(rawFolded, {});
    for (const QString &altGenus : parsed.altGenera) {
        if (altGenus.compare(parsed.genus, Qt::CaseInsensitive) == 0 || parsed.specificEpithet.isEmpty())
            continue;
        lookup((altGenus + QLatin1Char(' ') + parsed.specificEpithet).toLower(), {});
    }
    if (parsed.isGenusOnly() && !parsed.genus.isEmpty())
        lookup(parsed.genus.toLower(), QStringLiteral("genus"));

    return out;
}

QList<TaxonCandidate> TaxonResolver::fuzzyCandidates(const ParsedName &parsed) const
{
    if (parsed.genus.isEmpty())
        return {};

    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    const QString target = parsed.hasSpecies() ? parsed.foldedBinomial()
                                               : parsed.genus.toLower();

    QHash<qint64, TaxonCandidate> byTaxon;

    auto consider = [&](QSqlQuery &q) {
        while (q.next()) {
            const QString folded = q.value(0).toString();
            const double sim = nameSimilarity(target, folded);
            if (sim < m_fuzzyThreshold)
                continue;
            TaxonCandidate c;
            c.matchedVia = QStringLiteral("fuzzy");
            c.taxonId = q.value(1).toLongLong();
            c.inatId = q.value(2).toLongLong();
            c.name = q.value(3).toString();
            c.rank = q.value(4).toString();
            c.commonName = q.value(5).toString();
            c.score = sim * 0.74;
            auto it = byTaxon.find(c.taxonId);
            if (it == byTaxon.end() || it->score < c.score)
                byTaxon.insert(c.taxonId, c);
        }
    };

    const QString selectCols = QStringLiteral(
        "SELECT tn.name_folded, t.id, t.inat_id, t.name, t.rank, t.common_name "
        "FROM taxon_name tn JOIN taxon t ON t.id = tn.taxon_id ");

    // Genus-prefix anchored.
    {
        QSqlQuery q(db);
        q.prepare(selectCols + QStringLiteral("WHERE tn.name_folded LIKE ? LIMIT 400"));
        q.addBindValue(parsed.genus.left(3).toLower() + QLatin1Char('%'));
        if (q.exec())
            consider(q);
    }
    // Epithet anchored (catches a mistyped genus with a correct epithet).
    if (parsed.hasSpecies()) {
        QSqlQuery q(db);
        q.prepare(selectCols + QStringLiteral(
            "WHERE tn.name_folded LIKE ? AND t.rank IN ('species','subspecies','variety','form') "
            "LIMIT 400"));
        q.addBindValue(QStringLiteral("% ") + parsed.specificEpithet.toLower());
        if (q.exec())
            consider(q);
    }

    return byTaxon.values();
}

void TaxonResolver::applyHints(QList<TaxonCandidate> &candidates, const ResolveHints &hints) const
{
    if (hints.genus.isEmpty())
        return;
    const QString hintGenus = hints.genus.toLower();

    // A "genus" that isn't one in the taxonomy cache says nothing either way:
    // folder classification guesses genus for any single capitalised word, so
    // group folders like "Cuckoos" or "Fairywrens" would otherwise count
    // against every photo filed in them.
    {
        QSqlQuery q(QSqlDatabase::database(m_connectionName, false));
        q.prepare(QStringLiteral(
            "SELECT 1 FROM taxon_name tn JOIN taxon t ON t.id = tn.taxon_id "
            "WHERE tn.name_folded = ? AND t.rank = 'genus' LIMIT 1"));
        q.addBindValue(TaxonomyStore::foldName(hints.genus));
        if (!q.exec() || !q.next())
            return;
    }

    for (TaxonCandidate &c : candidates) {
        if (c.matchedVia == QLatin1String("vernacular") && !hints.genusFromFolder)
            continue;
        const QString candGenus = genusOf(c.name).toLower();
        if (candGenus == hintGenus) {
            c.score = std::min(1.0, c.score + 0.03);
        } else if (c.matchedVia != QLatin1String("synonym")
                   && c.matchedVia != QLatin1String("alias")) {
            // The folder's genus disagrees and this wasn't a synonym resolution.
            c.score -= 0.15;
        }
    }
}

QList<TaxonCandidate> TaxonResolver::resolve(const ParsedName &parsed,
                                             const ResolveHints &hints) const
{
    if (parsed.raw.isEmpty())
        return {};

    QList<TaxonCandidate> candidates = exactCandidates(parsed);
    const bool hadExact = !candidates.isEmpty();
    if (!hadExact)
        candidates = fuzzyCandidates(parsed);

    applyHints(candidates, hints);

    // Dedupe by taxon, keeping the best score.
    QHash<qint64, TaxonCandidate> best;
    for (const TaxonCandidate &c : candidates) {
        auto it = best.find(c.taxonId);
        if (it == best.end() || it->score < c.score)
            best.insert(c.taxonId, c);
    }

    QList<TaxonCandidate> result = best.values();
    if (!hints.preferTaxonIds.isEmpty()) {
        const bool anyPreferred = std::any_of(result.cbegin(), result.cend(), [&](const auto &c) {
            return hints.preferTaxonIds.contains(c.taxonId);
        });
        if (anyPreferred)
            result.removeIf([&](const TaxonCandidate &c) {
                return !hints.preferTaxonIds.contains(c.taxonId);
            });
    }
    std::sort(result.begin(), result.end(),
              [](const TaxonCandidate &a, const TaxonCandidate &b) { return a.score > b.score; });
    if (result.size() > 8)
        result.resize(8);
    return result;
}

} // namespace pl::match
