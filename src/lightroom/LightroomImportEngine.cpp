#include "lightroom/LightroomImportEngine.h"

#include "db/Database.h"
#include "match/NameParser.h"
#include "match/TaxonResolver.h"

#include <QDir>
#include <QHash>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include <optional>

namespace pl::lightroom {
namespace {

QString normalisePath(const QString &path)
{
    return QDir::cleanPath(QDir::fromNativeSeparators(path));
}

constexpr int kCommitEvery = 100;

} // namespace

LightroomImportEngine::LightroomImportEngine(QString connectionName)
    : m_connectionName(std::move(connectionName))
{
}

LightroomImportEngine::Stats LightroomImportEngine::import(
    const QList<LightroomPhotoKeywords> &photos, const CancelFn &cancel,
    const ProgressFn &progress)
{
    Stats stats;

    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    if (!db.isOpen()) {
        stats.error = QStringLiteral("catalogue connection is not open");
        return stats;
    }

    // Every rendition's path, normalised the same way scan::CatalogueWriter
    // stores it (QDir::cleanPath), so a Lightroom-reconstructed path matches
    // regardless of separator style.
    QHash<QString, qint64> captureIdByPath;
    {
        QSqlQuery q(db);
        q.setForwardOnly(true);
        if (!q.exec(QStringLiteral("SELECT path, capture_id FROM rendition"))) {
            stats.error = q.lastError().text();
            return stats;
        }
        while (q.next())
            captureIdByPath.insert(normalisePath(q.value(0).toString()), q.value(1).toLongLong());
    }

    match::TaxonResolver resolver(m_connectionName);
    QHash<QString, std::optional<match::TaxonCandidate>> resolvedKeywords;

    // A keyword string recurs across many photos; resolve each distinct one
    // once rather than once per photo.
    auto resolveKeyword = [&](const QString &keyword) -> std::optional<match::TaxonCandidate> {
        auto it = resolvedKeywords.find(keyword);
        if (it != resolvedKeywords.end())
            return it.value();

        std::optional<match::TaxonCandidate> resolved;
        const match::ParsedName parsed = match::parseName(keyword);
        if (parsed.hasGenus()) {
            const auto candidates = resolver.resolve(parsed);
            if (!candidates.isEmpty())
                resolved = candidates.first();
        }
        resolvedKeywords.insert(keyword, resolved);
        if (resolved)
            ++stats.keywordsResolved;
        return resolved;
    };

    if (!db.transaction()) {
        stats.error = db.lastError().text();
        return stats;
    }

    const QString nowExpr = Database::nowIsoExpr(Database::backendFor(m_connectionName));
    int done = 0;
    int sinceCommit = 0;
    for (const LightroomPhotoKeywords &photo : photos) {
        if (cancel && cancel()) {
            stats.cancelled = true;
            break;
        }

        const auto captureIt = captureIdByPath.constFind(normalisePath(photo.absolutePath));
        if (captureIt == captureIdByPath.constEnd()) {
            ++stats.photosUnmatched;
        } else {
            ++stats.photosMatched;
            const qint64 captureId = captureIt.value();

            for (const QString &keyword : photo.keywords) {
                const auto resolved = resolveKeyword(keyword);
                if (!resolved)
                    continue;   // noise (locality, event tag, ...) -- resolver found no taxon

                QSqlQuery ins(db);
                ins.prepare(QStringLiteral(
                    "INSERT INTO capture_keyword_hint "
                    "  (capture_id, source, raw_keyword, taxon_id, confidence, imported_at) "
                    "VALUES (?, 'lightroom', ?, ?, ?, %1) "
                    "ON CONFLICT(capture_id, source, raw_keyword) DO UPDATE SET "
                    "  taxon_id = excluded.taxon_id, confidence = excluded.confidence, "
                    "  imported_at = excluded.imported_at")
                        .arg(nowExpr));
                ins.addBindValue(qlonglong(captureId));
                ins.addBindValue(keyword);
                ins.addBindValue(qlonglong(resolved->taxonId));
                ins.addBindValue(resolved->score);
                if (!ins.exec()) {
                    db.rollback();
                    stats.error = ins.lastError().text();
                    return stats;
                }
                ++stats.hintsWritten;
                if (++sinceCommit >= kCommitEvery) {
                    if (!db.commit() || !db.transaction()) {
                        stats.error = db.lastError().text();
                        return stats;
                    }
                    sinceCommit = 0;
                }
            }
        }

        ++done;
        if (progress && (done % 100 == 0 || done == photos.size()))
            progress(done, photos.size());
    }

    if (!db.commit()) {
        stats.error = db.lastError().text();
        return stats;
    }
    if (progress)
        progress(done, photos.size());
    return stats;
}

} // namespace pl::lightroom
