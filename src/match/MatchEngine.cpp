#include "match/MatchEngine.h"

#include "match/NameParser.h"
#include "match/PathClassifier.h"
#include "match/TaxonResolver.h"

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include <algorithm>

namespace pl::match {
namespace {

struct FolderContext
{
    QString path;
    QString kind;
    QString rank;
    QString inferredName;
    QString genusHint;
    QString familyHint;
};

FolderContext loadFolderContext(QSqlDatabase &db, int folderId)
{
    FolderContext ctx;
    int cur = folderId;
    bool first = true;
    while (cur > 0) {
        QSqlQuery q(db);
        q.prepare(QStringLiteral(
            "SELECT parent_id, path, kind, inferred_rank, inferred_name FROM folder WHERE id = ?"));
        q.addBindValue(cur);
        if (!q.exec() || !q.next())
            break;

        const int parentId = q.value(0).isNull() ? -1 : q.value(0).toInt();
        const QString kind = q.value(2).toString();
        const QString rank = q.value(3).toString();
        const QString name = q.value(4).toString();

        if (first) {
            ctx.path = q.value(1).toString();
            ctx.kind = kind;
            ctx.rank = rank;
            ctx.inferredName = name;
            first = false;
        }
        if (ctx.genusHint.isEmpty() && rank == QLatin1String("genus"))
            ctx.genusHint = name;
        if (ctx.familyHint.isEmpty() && rank == QLatin1String("family"))
            ctx.familyHint = name;
        cur = parentId;
    }
    return ctx;
}

QString qualifierTag(Qualifier q)
{
    switch (q) {
    case Qualifier::Sp:           return QStringLiteral("sp");
    case Qualifier::SpAff:        return QStringLiteral("aff");
    case Qualifier::SensuLato:    return QStringLiteral("s.l.");
    case Qualifier::SensuStricto: return QStringLiteral("s.s.");
    case Qualifier::Aggregate:    return QStringLiteral("agg");
    case Qualifier::Hybrid:       return QStringLiteral("hybrid");
    case Qualifier::Undescribed:  return QStringLiteral("undescribed");
    case Qualifier::Unidentified: return QStringLiteral("unidentified");
    case Qualifier::None:         return {};
    }
    return {};
}

bool wantsGenusLevel(Qualifier q)
{
    switch (q) {
    case Qualifier::Sp:
    case Qualifier::SpAff:
    case Qualifier::Aggregate:
    case Qualifier::Hybrid:
    case Qualifier::Undescribed:
    case Qualifier::Unidentified:
        return true;
    default:
        return false;
    }
}

ParsedName genusOnly(const QString &genus)
{
    ParsedName p;
    p.raw = genus;
    p.genus = genus;
    return p;
}

} // namespace

MatchEngine::MatchEngine(QString connectionName)
    : m_connectionName(std::move(connectionName))
{
}

MatchOutcome MatchEngine::evaluateCapture(qint64 captureId) const
{
    MatchOutcome out;
    out.captureId = captureId;
    out.status = QStringLiteral("pending");

    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);

    QSqlQuery cap(db);
    cap.prepare(QStringLiteral(
        "SELECT folder_id, base_name, name_text FROM capture WHERE id = ?"));
    cap.addBindValue(qlonglong(captureId));
    if (!cap.exec() || !cap.next()) {
        out.note = QStringLiteral("capture not found");
        return out;
    }
    const int folderId = cap.value(0).toInt();
    const QString baseName = cap.value(1).toString();
    const QString nameText = cap.value(2).toString();

    const FolderContext folder = loadFolderContext(db, folderId);

    if (folder.kind == QLatin1String("staging")) {
        out.note = QStringLiteral("staging folder");
        return out;
    }

    TaxonResolver resolver(m_connectionName);

    const QString rawForAlias = !nameText.isEmpty() ? nameText : folder.inferredName;
    if (!rawForAlias.isEmpty() && resolver.isMarkedNonTaxon(rawForAlias, folder.path)) {
        out.note = QStringLiteral("marked not a taxon");
        return out;
    }

    const ParsedName fileParsed = parseName(nameText);
    const ParsedName folderParsed =
        folder.kind == QLatin1String("taxon") ? parseName(folder.inferredName) : ParsedName{};

    ResolveHints hints;
    hints.genus = !folder.genusHint.isEmpty() ? folder.genusHint
                  : (!folderParsed.genus.isEmpty() ? folderParsed.genus : fileParsed.genus);
    hints.family = folder.familyHint;
    hints.folderPath = folder.path;

    const Qualifier qualifier =
        fileParsed.qualifier != Qualifier::None ? fileParsed.qualifier : folderParsed.qualifier;
    out.qualifier = qualifierTag(qualifier);

    // sp. / agg / aff / hybrid / undescribed / unidentified: match at genus.
    if (wantsGenusLevel(qualifier)) {
        const QString genus = !fileParsed.genus.isEmpty() ? fileParsed.genus : folderParsed.genus;
        if (genus.isEmpty()) {
            out.note = QStringLiteral("qualified name without a genus");
            return out;
        }
        const auto cands = resolver.resolve(genusOnly(genus), hints);
        if (cands.isEmpty()) {
            out.note = QStringLiteral("genus not in the taxonomy cache");
            return out;
        }
        const TaxonCandidate &top = cands.first();
        out.taxonId = top.taxonId;
        out.matchedRank = top.rank;
        out.method = QStringLiteral("file");
        const bool cleanGenus = top.score >= 0.98
                                && (qualifier == Qualifier::Sp || qualifier == Qualifier::Aggregate);
        out.confidence = top.score * (cleanGenus ? 0.95 : 0.7);
        out.status = cleanGenus ? QStringLiteral("auto") : QStringLiteral("pending");
        out.note = QStringLiteral("matched at genus (%1)").arg(out.qualifier);
        return out;
    }

    const auto fileCands = fileParsed.hasGenus() ? resolver.resolve(fileParsed, hints)
                                                 : QList<TaxonCandidate>{};
    const auto folderCands = folderParsed.hasGenus() ? resolver.resolve(folderParsed, hints)
                                                     : QList<TaxonCandidate>{};

    const TaxonCandidate *topFile = fileCands.isEmpty() ? nullptr : &fileCands.first();
    const TaxonCandidate *topFolder = folderCands.isEmpty() ? nullptr : &folderCands.first();

    if (topFile && topFolder && topFile->taxonId == topFolder->taxonId) {
        out.taxonId = topFile->taxonId;
        out.matchedRank = topFile->rank;
        out.method = QStringLiteral("folder+file");
        out.confidence = std::min(1.0, std::max(topFile->score, topFolder->score) + 0.05);
        out.note = topFile->matchedVia == QLatin1String("synonym")
                       ? QStringLiteral("folder and filename reconcile via a synonym")
                       : QString();
    } else if (topFile && topFolder && topFile->taxonId != topFolder->taxonId) {
        const TaxonCandidate *pick = topFile->score >= topFolder->score ? topFile : topFolder;
        out.taxonId = pick->taxonId;
        out.matchedRank = pick->rank;
        out.method = QStringLiteral("folder+file");
        out.confidence = pick->score * 0.6;
        out.note = QStringLiteral("folder and filename disagree");
    } else if (topFile) {
        out.taxonId = topFile->taxonId;
        out.matchedRank = topFile->rank;
        out.method = topFile->matchedVia == QLatin1String("fuzzy") ? QStringLiteral("fuzzy")
                     : topFile->matchedVia == QLatin1String("alias") ? QStringLiteral("alias")
                                                                     : QStringLiteral("file");
        out.confidence = topFile->score * (topFile->matchedVia == QLatin1String("fuzzy") ? 1.0 : 0.98);
    } else if (topFolder) {
        out.taxonId = topFolder->taxonId;
        out.matchedRank = topFolder->rank;
        out.method = topFolder->matchedVia == QLatin1String("fuzzy") ? QStringLiteral("fuzzy")
                                                                     : QStringLiteral("folder");
        out.confidence = topFolder->score * 0.9;
    } else {
        out.note = fileParsed.hasGenus() || folderParsed.hasGenus()
                       ? QStringLiteral("no taxonomy match — is a project covering this group built?")
                       : QStringLiteral("no name to match");
        return out;
    }

    out.status = out.confidence >= m_autoThreshold ? QStringLiteral("auto")
                                                   : QStringLiteral("pending");
    return out;
}

MatchEngine::Stats MatchEngine::matchAll(const CancelFn &cancel, const ProgressFn &progress)
{
    Stats stats;

    classifyFolders(m_connectionName);

    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    if (!db.isOpen()) {
        stats.error = QStringLiteral("catalogue connection is not open");
        return stats;
    }

    QList<qint64> captureIds;
    {
        QSqlQuery q(db);
        q.setForwardOnly(true);
        if (!q.exec(QStringLiteral("SELECT id FROM capture"))) {
            stats.error = q.lastError().text();
            return stats;
        }
        while (q.next())
            captureIds.append(q.value(0).toLongLong());
    }
    stats.captures = int(captureIds.size());

    if (!db.transaction()) {
        stats.error = db.lastError().text();
        return stats;
    }

    int done = 0;
    for (qint64 id : captureIds) {
        if (cancel && cancel()) {
            stats.cancelled = true;
            break;
        }

        // A capture the user has already decided is left untouched.
        QSqlQuery userRow(db);
        userRow.prepare(QStringLiteral(
            "SELECT 1 FROM capture_match WHERE capture_id = ? AND decided_by = 'user' LIMIT 1"));
        userRow.addBindValue(qlonglong(id));
        if (userRow.exec() && userRow.next()) {
            ++done;
            continue;
        }

        const MatchOutcome outcome = evaluateCapture(id);

        QSqlQuery del(db);
        del.prepare(QStringLiteral(
            "DELETE FROM capture_match WHERE capture_id = ? AND decided_by = 'engine'"));
        del.addBindValue(qlonglong(id));
        del.exec();

        QSqlQuery ins(db);
        ins.prepare(QStringLiteral(
            "INSERT INTO capture_match (capture_id, taxon_id, matched_rank, method, confidence, "
            "status, qualifier, note, decided_by) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, 'engine')"));
        ins.addBindValue(qlonglong(id));
        ins.addBindValue(outcome.taxonId > 0 ? QVariant(qlonglong(outcome.taxonId)) : QVariant());
        ins.addBindValue(outcome.matchedRank.isEmpty() ? QVariant() : outcome.matchedRank);
        ins.addBindValue(outcome.method.isEmpty() ? QStringLiteral("none") : outcome.method);
        ins.addBindValue(outcome.confidence);
        ins.addBindValue(outcome.status);
        ins.addBindValue(outcome.qualifier.isEmpty() ? QVariant() : outcome.qualifier);
        ins.addBindValue(outcome.note.isEmpty() ? QVariant() : outcome.note);
        if (!ins.exec()) {
            db.rollback();
            stats.error = ins.lastError().text();
            return stats;
        }

        if (outcome.status == QLatin1String("auto"))
            ++stats.autoApplied;
        else
            ++stats.pending;
        if (!outcome.hasTaxon())
            ++stats.unmatched;

        if (progress && (++done % 200) == 0)
            progress(done, stats.captures);
    }

    if (!db.commit()) {
        stats.error = db.lastError().text();
        return stats;
    }
    if (progress)
        progress(done, stats.captures);
    return stats;
}

} // namespace pl::match
