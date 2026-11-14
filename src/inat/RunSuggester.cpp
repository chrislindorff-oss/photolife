#include "inat/RunSuggester.h"

#include "net/INatClient.h"
#include "taxonomy/TaxonomyStore.h"

#include <QPointer>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>

#include <algorithm>

namespace pl::inat {

RunSuggester::RunSuggester(net::INatClient &inat, PhotoIdentifier &identifier, QString connectionName,
                           QObject *parent)
    : QObject(parent), m_inat(inat), m_identifier(identifier), m_connectionName(std::move(connectionName))
{
    connect(&m_identifier, &PhotoIdentifier::finished, this, &RunSuggester::identified);
}

QList<RunSuggester::Photo> RunSuggester::loadPhotos(const QString &connectionName, const QList<qint64> &captureIds)
{
    QList<Photo> photos;
    if (captureIds.isEmpty())
        return photos;
    QStringList ids;
    for (qint64 id : captureIds)
        ids << QString::number(id);
    QSqlQuery q(QSqlDatabase::database(connectionName, false));
    q.setForwardOnly(true);
    // The same preview the review queue shows: a JPEG before a RAW.
    if (!q.exec(QStringLiteral(
            "SELECT c.id, c.captured_on, c.latitude, c.longitude, "
            "  (SELECT r.path FROM rendition r WHERE r.capture_id = c.id "
            "     ORDER BY (r.kind='raw'), r.id LIMIT 1) "
            "FROM capture c WHERE c.id IN (%1)")
                    .arg(ids.join(QLatin1Char(',')))))
        return photos;
    while (q.next()) {
        Photo p;
        p.captureId = q.value(0).toLongLong();
        const QString when = q.value(1).toString();
        p.day = QDate::fromString(when.left(10), Qt::ISODate);
        if (when.size() > 10)   // a time, not just the day
            p.taken = QDateTime::fromString(when.left(19), Qt::ISODate);
        if (!q.value(2).isNull() && !q.value(3).isNull())
            p.lonLat = QPointF(q.value(3).toDouble(), q.value(2).toDouble());
        p.path = q.value(4).toString();
        if (!p.path.isEmpty())
            photos << p;
    }
    return photos;
}

QList<QList<RunSuggester::Photo>> RunSuggester::splitRuns(QList<Photo> photos, int gapSeconds)
{
    std::stable_sort(photos.begin(), photos.end(), [](const Photo &a, const Photo &b) {
        if (a.taken.isValid() != b.taken.isValid())
            return a.taken.isValid();   // the timed ones first
        if (a.taken.isValid())
            return a.taken < b.taken;
        return a.captureId < b.captureId;
    });
    QList<QList<Photo>> runs;
    for (const Photo &p : std::as_const(photos)) {
        const bool joins = !runs.isEmpty() && p.taken.isValid() && runs.last().last().taken.isValid()
                           && runs.last().last().taken.secsTo(p.taken) <= gapSeconds;
        if (joins)
            runs.last() << p;
        else
            runs << QList<Photo>{p};
    }
    return runs;
}

bool RunSuggester::recordSuggestion(const QString &connectionName, qint64 captureId, qint64 taxonInatId,
                                    double confidence, const QString &note)
{
    QSqlDatabase db = QSqlDatabase::database(connectionName, false);
    QSqlQuery known(db);
    known.prepare(QStringLiteral("SELECT id, rank FROM taxon WHERE inat_id = ?"));
    known.addBindValue(qlonglong(taxonInatId));
    if (!known.exec() || !known.next())
        return false;
    const qint64 taxonId = known.value(0).toLongLong();
    const QString rank = known.value(1).toString();
    // Already decided by hand meanwhile: leave it be.
    QSqlQuery user(db);
    user.prepare(QStringLiteral("SELECT 1 FROM capture_match WHERE capture_id = ? AND decided_by = 'user'"));
    user.addBindValue(qlonglong(captureId));
    if (user.exec() && user.next())
        return false;
    if (!db.transaction())
        return false;
    QSqlQuery del(db);
    del.prepare(QStringLiteral("DELETE FROM capture_match WHERE capture_id = ? AND (decided_by = 'vision' "
                               "OR taxon_id = ?)"));
    del.addBindValue(qlonglong(captureId));
    del.addBindValue(qlonglong(taxonId));
    del.exec();
    QSqlQuery ins(db);
    ins.prepare(QStringLiteral(
        "INSERT INTO capture_match (capture_id, taxon_id, matched_rank, method, confidence, status, note, "
        "decided_by) VALUES (?, ?, ?, 'vision', ?, 'pending', ?, 'vision')"));
    ins.addBindValue(qlonglong(captureId));
    ins.addBindValue(qlonglong(taxonId));
    ins.addBindValue(rank.isEmpty() ? QVariant() : rank);
    ins.addBindValue(confidence);
    ins.addBindValue(note.isEmpty() ? QVariant() : note);
    if (!ins.exec()) {
        db.rollback();
        return false;
    }
    return db.commit();
}

void RunSuggester::start(const QList<qint64> &captureIds, int gapSeconds)
{
    if (m_running)
        return;
    m_runs = splitRuns(loadPhotos(m_connectionName, captureIds), gapSeconds);
    m_result = {};
    m_at = -1;
    m_running = true;
    next();
}

void RunSuggester::cancel()
{
    if (!m_running)
        return;
    m_result.cancelled = true;
    finish();
}

void RunSuggester::next()
{
    if (!m_running)
        return;
    ++m_at;
    emit progress(m_at, int(m_runs.size()));
    if (m_at >= m_runs.size()) {
        finish();
        return;
    }
    const QList<Photo> &run = m_runs.at(m_at);
    const Photo &middle = run.at(run.size() / 2);
    IdentifyRequest request;
    request.key = middle.path;
    request.path = middle.path;
    request.lonLat = middle.lonLat;
    if (middle.day.isValid())
        request.date = middle.day;
    m_askingKey = middle.path;
    ++m_result.runs;
    m_identifier.identify(request);   // may answer straight away, from its cache
}

void RunSuggester::identified(const QString &key, const std::optional<QRectF> &crop, const Identification &result)
{
    if (!m_running || crop || key != m_askingKey || m_at < 0 || m_at >= m_runs.size())
        return;
    m_askingKey.clear();
    if (!result.ok()) {
        m_result.error = result.error;
        if (result.status == 401 || result.status == 403 || result.status == 429) {
            m_result.status = result.status;
            --m_result.runs;   // not asked, in effect
            finish();
            return;
        }
        ++m_result.failed;
        QMetaObject::invokeMethod(this, &RunSuggester::next, Qt::QueuedConnection);
        return;
    }
    if (result.result.suggestions.isEmpty()) {
        ++m_result.noSuggestion;
        QMetaObject::invokeMethod(this, &RunSuggester::next, Qt::QueuedConnection);
        return;
    }
    const net::VisionSuggestion top = result.result.suggestions.first();
    const QList<Photo> run = m_runs.at(m_at);
    taxonomy::TaxonomyStore store(m_connectionName);
    if (store.taxonLocalId(top.taxon.inatId)) {
        record(run, top);
        QMetaObject::invokeMethod(this, &RunSuggester::next, Qt::QueuedConnection);
        return;
    }
    // Not in the taxonomy yet: fetch it with its ancestors, so it sits in the
    // right place once confirmed (it isn't added to any reference tree).
    m_inat.fetchTaxon(top.taxon.inatId, [this, self = QPointer<RunSuggester>(this), run, top](
                                            net::Outcome<net::TaxonDetail> out) {
        if (!self || !m_running)
            return;
        if (out.ok()) {
            taxonomy::TaxonomyStore store(m_connectionName);
            for (const taxonomy::Taxon &a : out.value.ancestors) {
                if (!store.taxonLocalId(a.inatId))
                    store.upsertTaxon(a);
            }
            store.upsertTaxon(out.value.taxon);
            record(run, top);
        } else {
            ++m_result.failed;
            m_result.error = out.error;
        }
        next();
    });
}

void RunSuggester::record(const QList<Photo> &run, const net::VisionSuggestion &top)
{
    const QString note = tr("iNaturalist suggestion, %1%").arg(qRound(top.score));
    for (const Photo &p : run) {
        if (recordSuggestion(m_connectionName, p.captureId, top.taxon.inatId, top.score / 100.0, note))
            ++m_result.photos;
    }
}

void RunSuggester::finish()
{
    m_running = false;
    m_askingKey.clear();
    m_runs.clear();
    m_at = -1;
    emit finished(m_result);
}

} // namespace pl::inat
