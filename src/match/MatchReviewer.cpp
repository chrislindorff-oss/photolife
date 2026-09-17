#include "match/MatchReviewer.h"

#include "db/Database.h"
#include "match/NameParser.h"
#include "taxonomy/TaxonomyStore.h"

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>
#include <QVariant>

namespace pl::match {
namespace {
using taxonomy::TaxonomyStore;
} // namespace

MatchReviewer::MatchReviewer(QString connectionName)
    : m_connectionName(std::move(connectionName))
{
}

bool MatchReviewer::fail(const QString &what)
{
    m_error = what;
    return false;
}

MatchReviewer::CaptureInfo MatchReviewer::captureInfo(qint64 captureId) const
{
    CaptureInfo info;
    QSqlQuery q(QSqlDatabase::database(m_connectionName, false));
    q.prepare(QStringLiteral(
        "SELECT c.folder_id, c.name_text, f.name, f.path "
        "FROM capture c JOIN folder f ON f.id = c.folder_id WHERE c.id = ?"));
    q.addBindValue(qlonglong(captureId));
    if (q.exec() && q.next()) {
        info.folderId = q.value(0).toInt();
        info.nameText = q.value(1).toString();
        info.folderName = q.value(2).toString();
        info.folderPath = q.value(3).toString();
        info.found = true;
    }
    return info;
}

void MatchReviewer::learnAlias(const QString &rawText, qint64 taxonInatId, const QString &scope)
{
    if (rawText.trimmed().isEmpty())
        return;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "INSERT INTO name_alias (raw_text, raw_folded, taxon_id, scope, not_a_taxon) "
        "VALUES (?, ?, (SELECT id FROM taxon WHERE inat_id = ?), ?, 0) "
        "ON CONFLICT(raw_folded, scope) DO UPDATE SET "
        "  raw_text = excluded.raw_text, taxon_id = excluded.taxon_id, not_a_taxon = 0"));
    q.addBindValue(rawText.trimmed());
    q.addBindValue(TaxonomyStore::foldName(rawText));
    q.addBindValue(qlonglong(taxonInatId));
    q.addBindValue(scope.isEmpty() ? QStringLiteral("global") : scope);
    q.exec();
}

bool MatchReviewer::writeDecision(qint64 captureId, qint64 taxonInatId, const QString &status,
                                  const QString &matchedRank, const QString &qualifier)
{
    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);

    // A capture has exactly one live decision. Clear whatever it had before —
    // a stale engine guess, or an earlier user decision this one supersedes
    // (e.g. reassigning an already-confirmed capture to a different taxon) —
    // then write the new one fresh. The delete-then-insert pair is wrapped in
    // a transaction so two collaborators deciding the same capture at once
    // (shared Postgres catalogue) can't interleave and leave two live rows;
    // whichever transaction commits last simply wins.
    if (!db.transaction())
        return fail(db.lastError().text());

    QSqlQuery del(db);
    del.prepare(QStringLiteral("DELETE FROM capture_match WHERE capture_id = ?"));
    del.addBindValue(qlonglong(captureId));
    del.exec();

    QSqlQuery ins(db);
    ins.prepare(QStringLiteral(
        "INSERT INTO capture_match (capture_id, taxon_id, matched_rank, method, confidence, "
        "status, qualifier, decided_by, decided_at) "
        "VALUES (?, (SELECT id FROM taxon WHERE inat_id = ?), ?, 'manual', 1.0, ?, ?, 'user', %1)")
                    .arg(Database::nowIsoExpr(Database::backendFor(m_connectionName))));
    ins.addBindValue(qlonglong(captureId));
    ins.addBindValue(taxonInatId > 0 ? QVariant(qlonglong(taxonInatId)) : QVariant());
    ins.addBindValue(matchedRank.isEmpty() ? QVariant() : matchedRank);
    ins.addBindValue(status);
    ins.addBindValue(qualifier.isEmpty() ? QVariant() : qualifier);
    if (!ins.exec()) {
        const QString error = ins.lastError().text();
        db.rollback();
        return fail(error);
    }

    if (!db.commit())
        return fail(db.lastError().text());
    return true;
}

bool MatchReviewer::confirm(qint64 captureId, qint64 taxonInatId, bool learnAliasToo)
{
    m_error.clear();
    const CaptureInfo info = captureInfo(captureId);
    if (!info.found)
        return fail(QStringLiteral("capture not found"));

    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);

    qint64 targetInat = taxonInatId;
    QString rank;
    if (targetInat <= 0) {
        QSqlQuery q(db);
        q.prepare(QStringLiteral(
            "SELECT t.inat_id, t.rank FROM capture_match m JOIN taxon t ON t.id = m.taxon_id "
            "WHERE m.capture_id = ? ORDER BY (m.decided_by = 'user') DESC, m.confidence DESC "
            "LIMIT 1"));
        q.addBindValue(qlonglong(captureId));
        if (q.exec() && q.next()) {
            targetInat = q.value(0).toLongLong();
            rank = q.value(1).toString();
        }
    } else {
        QSqlQuery q(db);
        q.prepare(QStringLiteral("SELECT rank FROM taxon WHERE inat_id = ?"));
        q.addBindValue(qlonglong(targetInat));
        if (q.exec() && q.next())
            rank = q.value(0).toString();
    }

    if (targetInat <= 0)
        return fail(QStringLiteral("no taxon to confirm — reassign it first"));

    if (!writeDecision(captureId, targetInat, QStringLiteral("confirmed"), rank, {}))
        return false;

    if (learnAliasToo) {
        const QString raw = !info.nameText.isEmpty() ? info.nameText : info.folderName;
        learnAlias(raw, targetInat, QStringLiteral("global"));
    }
    return true;
}

bool MatchReviewer::setGenusOnly(qint64 captureId, qint64 genusInatId)
{
    m_error.clear();
    if (genusInatId <= 0)
        return fail(QStringLiteral("no genus given"));
    if (!captureInfo(captureId).found)
        return fail(QStringLiteral("capture not found"));
    return writeDecision(captureId, genusInatId, QStringLiteral("confirmed"),
                         QStringLiteral("genus"), QStringLiteral("sp"));
}

bool MatchReviewer::reject(qint64 captureId)
{
    m_error.clear();
    if (!captureInfo(captureId).found)
        return fail(QStringLiteral("capture not found"));
    return writeDecision(captureId, 0, QStringLiteral("rejected"), {}, {});
}

bool MatchReviewer::markNotATaxon(qint64 captureId, bool folderScope)
{
    m_error.clear();
    const CaptureInfo info = captureInfo(captureId);
    if (!info.found)
        return fail(QStringLiteral("capture not found"));

    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    QSqlQuery alias(db);
    alias.prepare(QStringLiteral(
        "INSERT INTO name_alias (raw_text, raw_folded, taxon_id, scope, not_a_taxon) "
        "VALUES (?, ?, NULL, ?, 1) "
        "ON CONFLICT(raw_folded, scope) DO UPDATE SET not_a_taxon = 1, taxon_id = NULL"));
    alias.addBindValue(info.folderName);
    alias.addBindValue(taxonomy::TaxonomyStore::foldName(info.folderName));
    alias.addBindValue(folderScope ? info.folderPath : QStringLiteral("global"));
    if (!alias.exec())
        return fail(alias.lastError().text());

    if (folderScope) {
        QSqlQuery fk(db);
        fk.prepare(QStringLiteral("UPDATE folder SET kind = 'locality' WHERE id = ?"));
        fk.addBindValue(info.folderId);
        fk.exec();
    }

    return writeDecision(captureId, 0, QStringLiteral("rejected"), {}, {});
}

bool MatchReviewer::resetToPending(qint64 captureId)
{
    m_error.clear();
    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    QSqlQuery q(db);
    q.prepare(QStringLiteral("DELETE FROM capture_match WHERE capture_id = ?"));
    q.addBindValue(qlonglong(captureId));
    return q.exec() ? true : fail(q.lastError().text());
}

int MatchReviewer::applyTaxonToFolder(int folderId, qint64 taxonInatId, bool recursive,
                                      bool onlyPending)
{
    m_error.clear();
    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);

    QList<int> folders{folderId};
    if (recursive) {
        for (int i = 0; i < folders.size(); ++i) {
            QSqlQuery kids(db);
            kids.prepare(QStringLiteral("SELECT id FROM folder WHERE parent_id = ?"));
            kids.addBindValue(folders.at(i));
            if (kids.exec()) {
                while (kids.next())
                    folders.append(kids.value(0).toInt());
            }
        }
    }

    QStringList placeholders;
    for (int i = 0; i < folders.size(); ++i)
        placeholders << QStringLiteral("?");

    QString sql = QStringLiteral(
        "SELECT c.id FROM capture c WHERE c.folder_id IN (%1)").arg(placeholders.join(QLatin1Char(',')));
    if (onlyPending) {
        sql += QStringLiteral(
            " AND NOT EXISTS (SELECT 1 FROM capture_match m WHERE m.capture_id = c.id "
            "                 AND m.decided_by = 'user')");
    }

    QSqlQuery caps(db);
    caps.prepare(sql);
    for (int f : folders)
        caps.addBindValue(f);
    if (!caps.exec()) {
        fail(caps.lastError().text());
        return 0;
    }

    QList<qint64> captureIds;
    while (caps.next())
        captureIds.append(caps.value(0).toLongLong());

    int changed = 0;
    for (qint64 id : captureIds) {
        if (writeDecision(id, taxonInatId, QStringLiteral("confirmed"), {}, {}))
            ++changed;
    }
    return changed;
}

int MatchReviewer::ignoreFolderTree(int folderId)
{
    m_error.clear();
    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);

    QList<int> folders{folderId};
    for (int i = 0; i < folders.size(); ++i) {
        QSqlQuery kids(db);
        kids.prepare(QStringLiteral("SELECT id FROM folder WHERE parent_id = ?"));
        kids.addBindValue(folders.at(i));
        if (kids.exec()) {
            while (kids.next())
                folders.append(kids.value(0).toInt());
        }
    }

    int updated = 0;
    for (int f : folders) {
        QSqlQuery up(db);
        up.prepare(QStringLiteral("UPDATE folder SET kind = 'staging' WHERE id = ?"));
        up.addBindValue(f);
        if (up.exec())
            ++updated;

        QSqlQuery rej(db);
        rej.prepare(QStringLiteral(
            "UPDATE capture_match SET status = 'rejected', decided_by = 'user', "
            "  decided_at = %1 "
            "WHERE capture_id IN (SELECT id FROM capture WHERE folder_id = ?) "
            "  AND decided_by = 'engine'")
                        .arg(Database::nowIsoExpr(Database::backendFor(m_connectionName))));
        rej.addBindValue(f);
        rej.exec();
    }
    return updated;
}

} // namespace pl::match
