#include "match/CandidateFinder.h"

#include "match/NameParser.h"
#include "taxonomy/TaxonomyStore.h"

#include <QHash>
#include <QSqlDatabase>
#include <QSqlQuery>

#include <algorithm>

namespace pl::match {
namespace {

struct FolderCtx
{
    QString path;
    QString inferredName;
    QString kind;
    QString genusHint;
    QString familyHint;
};

FolderCtx loadFolderCtx(QSqlDatabase &db, int folderId)
{
    FolderCtx ctx;
    int cur = folderId;
    bool first = true;
    while (cur > 0) {
        QSqlQuery q(db);
        q.prepare(QStringLiteral(
            "SELECT parent_id, path, kind, inferred_rank, inferred_name FROM folder WHERE id = ?"));
        q.addBindValue(cur);
        if (!q.exec() || !q.next())
            break;
        const int parent = q.value(0).isNull() ? -1 : q.value(0).toInt();
        const QString rank = q.value(3).toString();
        const QString name = q.value(4).toString();
        if (first) {
            ctx.path = q.value(1).toString();
            ctx.kind = q.value(2).toString();
            ctx.inferredName = name;
            first = false;
        }
        if (ctx.genusHint.isEmpty() && rank == QLatin1String("genus"))
            ctx.genusHint = name;
        if (ctx.familyHint.isEmpty() && rank == QLatin1String("family"))
            ctx.familyHint = name;
        cur = parent;
    }
    return ctx;
}

} // namespace

CandidateFinder::CandidateFinder(QString connectionName)
    : m_connectionName(std::move(connectionName))
{
}

QList<TaxonCandidate> CandidateFinder::forCapture(qint64 captureId) const
{
    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);

    QSqlQuery cap(db);
    cap.prepare(QStringLiteral("SELECT folder_id, name_text FROM capture WHERE id = ?"));
    cap.addBindValue(qlonglong(captureId));
    if (!cap.exec() || !cap.next())
        return {};
    const int folderId = cap.value(0).toInt();
    const QString nameText = cap.value(1).toString();

    const FolderCtx folder = loadFolderCtx(db, folderId);

    const ParsedName fileParsed = parseName(nameText);
    const ParsedName folderParsed =
        folder.kind == QLatin1String("taxon") ? parseName(folder.inferredName) : ParsedName{};

    ResolveHints hints;
    hints.genus = !folder.genusHint.isEmpty() ? folder.genusHint
                  : (!folderParsed.genus.isEmpty() ? folderParsed.genus : fileParsed.genus);
    hints.family = folder.familyHint;
    hints.folderPath = folder.path;

    TaxonResolver resolver(m_connectionName);
    resolver.setFuzzyThreshold(0.62);   // widen for the review UI

    QHash<qint64, TaxonCandidate> best;
    auto merge = [&](const QList<TaxonCandidate> &list) {
        for (const TaxonCandidate &c : list) {
            auto it = best.find(c.taxonId);
            if (it == best.end() || it->score < c.score)
                best.insert(c.taxonId, c);
        }
    };
    if (fileParsed.hasGenus())
        merge(resolver.resolve(fileParsed, hints));
    if (folderParsed.hasGenus())
        merge(resolver.resolve(folderParsed, hints));

    QList<TaxonCandidate> result = best.values();
    std::sort(result.begin(), result.end(),
              [](const TaxonCandidate &a, const TaxonCandidate &b) { return a.score > b.score; });
    if (result.size() > 12)
        result.resize(12);
    return result;
}

QList<TaxonCandidate> CandidateFinder::search(const QString &text, int limit) const
{
    QList<TaxonCandidate> out;
    const QString folded = taxonomy::TaxonomyStore::foldSearchText(text);
    if (folded.isEmpty())
        return out;

    QSqlQuery q(QSqlDatabase::database(m_connectionName, false));
    q.prepare(QStringLiteral(
        "SELECT DISTINCT t.id, t.inat_id, t.name, t.rank, t.common_name, tn.kind, "
        "  (tn.name_folded = ?) AS exact, (tn.name_folded LIKE ?) AS prefix "
        "FROM taxon_name tn JOIN taxon t ON t.id = tn.taxon_id "
        "WHERE tn.name_folded LIKE ? "
        "ORDER BY exact DESC, prefix DESC, (tn.kind = 'accepted') DESC, length(t.name) "
        "LIMIT ?"));
    q.addBindValue(folded);
    q.addBindValue(folded + QLatin1Char('%'));
    q.addBindValue(QLatin1Char('%') + folded + QLatin1Char('%'));
    q.addBindValue(limit);
    if (!q.exec())
        return out;

    while (q.next()) {
        TaxonCandidate c;
        c.taxonId = q.value(0).toLongLong();
        c.inatId = q.value(1).toLongLong();
        c.name = q.value(2).toString();
        c.rank = q.value(3).toString();
        c.commonName = q.value(4).toString();
        c.matchedVia = q.value(5).toString();
        c.score = q.value(6).toInt() ? 1.0 : (q.value(7).toInt() ? 0.9 : 0.7);
        out.append(c);
    }
    return out;
}

qint64 CandidateFinder::folderGenusInatId(qint64 captureId) const
{
    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);

    QSqlQuery cap(db);
    cap.prepare(QStringLiteral("SELECT folder_id, name_text FROM capture WHERE id = ?"));
    cap.addBindValue(qlonglong(captureId));
    if (!cap.exec() || !cap.next())
        return 0;

    const FolderCtx folder = loadFolderCtx(db, cap.value(0).toInt());
    QString genus = folder.genusHint;
    if (genus.isEmpty())
        genus = parseName(cap.value(1).toString()).genus;
    if (genus.isEmpty())
        genus = parseName(folder.inferredName).genus;
    if (genus.isEmpty())
        return 0;

    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "SELECT t.inat_id FROM taxon_name tn JOIN taxon t ON t.id = tn.taxon_id "
        "WHERE tn.name_folded = ? AND t.rank = 'genus' LIMIT 1"));
    q.addBindValue(genus.toLower());
    if (q.exec() && q.next())
        return q.value(0).toLongLong();
    return 0;
}

} // namespace pl::match
