#include "inat/InatDuplicateCheck.h"

#include "net/INatClient.h"
#include "net/INatParse.h"

#include <QFileInfo>
#include <QPointer>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStringList>

#include <algorithm>

namespace pl::inat {

InatDuplicateCheck::InatDuplicateCheck(net::INatClient &inat, QString connectionName, QObject *parent)
    : QObject(parent), m_inat(inat), m_connectionName(std::move(connectionName))
{
}

QList<ExistingObservation> InatDuplicateCheck::fromLibrary(const QString &connectionName,
                                                           const QList<QPair<QDate, QDate>> &ranges)
{
    QList<ExistingObservation> out;
    QHash<qint64, int> byId;
    QSqlDatabase db = QSqlDatabase::database(connectionName, false);
    for (const auto &[from, to] : ranges) {
        QSqlQuery q(db);
        q.setForwardOnly(true);
        q.prepare(QStringLiteral(
            "SELECT c.inat_observation_id, c.captured_on, c.latitude, c.longitude, "
            "  t.inat_id, t.name, t.common_name, t.ancestry, "
            "  (SELECT r.path FROM rendition r WHERE r.capture_id = c.id ORDER BY (r.kind = 'raw'), r.id LIMIT 1), "
            "  (SELECT r.content_hash FROM rendition r WHERE r.capture_id = c.id "
            "     ORDER BY (r.kind = 'raw'), r.id LIMIT 1) "
            "FROM capture c "
            "LEFT JOIN capture_match m ON m.id = (SELECT id FROM capture_match WHERE capture_id = c.id "
            "  AND status <> 'rejected' AND taxon_id IS NOT NULL "
            "  ORDER BY (status = 'confirmed') DESC, (decided_by = 'user') DESC, confidence DESC LIMIT 1) "
            "LEFT JOIN taxon t ON t.id = m.taxon_id "
            "WHERE c.inat_observation_id IS NOT NULL "
            "  AND substr(c.captured_on, 1, 10) >= ? AND substr(c.captured_on, 1, 10) <= ? "
            "ORDER BY c.captured_on"));
        q.addBindValue(from.toString(Qt::ISODate));
        q.addBindValue(to.toString(Qt::ISODate));
        if (!q.exec())
            continue;
        while (q.next()) {
            const qint64 id = q.value(0).toLongLong();
            const bool fresh = !byId.contains(id);
            if (fresh) {
                byId.insert(id, int(out.size()));
                out.append(ExistingObservation());
            }
            ExistingObservation &e = out[byId.value(id)];
            if (fresh) {
                e.id = id;
                e.fromLibrary = true;
                const QString on = q.value(1).toString();
                e.date = QDate::fromString(on.left(10), Qt::ISODate);
                if (on.size() > 10)
                    e.time = QDateTime::fromString(on, Qt::ISODate);
                e.taxonId = q.value(4).toLongLong();
                const QString name = q.value(5).toString(), common = q.value(6).toString();
                e.taxonName = common.isEmpty() ? name : QStringLiteral("%1 · %2").arg(common, name);
                e.taxonAncestorIds = net::inat::ancestryIds(q.value(7).toString());
                e.taxonAncestorIds.removeAll(e.taxonId);
                e.libraryPath = q.value(8).toString();
                e.libraryHash = q.value(9).toString();
                e.libraryPhoto = QFileInfo(e.libraryPath).fileName();
            }
            if (!e.latitude && !q.value(2).isNull() && !q.value(3).isNull()) {
                e.latitude = q.value(2).toDouble();
                e.longitude = q.value(3).toDouble();
            }
        }
    }
    return out;
}

void InatDuplicateCheck::start(const QString &userLogin, const QList<Group> &groups)
{
    if (m_running)
        return;
    m_running = true;
    m_cancelled = false;
    m_login = userLogin;
    m_groups = groups;
    QList<ProposedObservation> proposed;
    for (const Group &g : groups)
        proposed << g.observation;
    m_ranges = DuplicateFinder::dateRanges(proposed);
    m_existing = fromLibrary(m_connectionName, m_ranges);
    m_range = 0;
    m_page = 1;
    if (m_login.isEmpty()) {
        finish(false, tr("there's no iNaturalist username. Enter it above, or Get Token."), 0);
        return;
    }
    emit progress(0, int(m_ranges.size()));
    fetchPage();
}

void InatDuplicateCheck::fetchPage()
{
    if (m_cancelled) {
        finish(false, tr("stopped."), 0);
        return;
    }
    if (m_range >= m_ranges.size()) {
        finish(true, {}, 200);
        return;
    }
    const auto [from, to] = m_ranges.at(m_range);
    QPointer<InatDuplicateCheck> self(this);
    m_inat.fetchObservations(
        m_login, {}, 0, from, to, m_page,
        [self](net::Outcome<net::ObservationPage> out) {
            if (!self)
                return;
            if (!out.ok()) {
                self->finish(false, out.error, out.status);
                return;
            }
            // iNaturalist's own answer replaces the library's for the same observation.
            QSet<qint64> fetched;
            for (const taxonomy::Observation &o : out.value.results)
                fetched.insert(o.id);
            self->m_existing.erase(std::remove_if(self->m_existing.begin(), self->m_existing.end(),
                                                  [&](const ExistingObservation &e) { return fetched.contains(e.id); }),
                                   self->m_existing.end());
            for (const taxonomy::Observation &o : out.value.results)
                self->m_existing << ExistingObservation::fromInat(o);
            const bool more = out.value.results.size() >= kPerPage
                              && self->m_page * kPerPage < out.value.totalResults && self->m_page < kMaxPages;
            if (more) {
                ++self->m_page;
            } else {
                ++self->m_range;
                self->m_page = 1;
                emit self->progress(self->m_range, int(self->m_ranges.size()));
            }
            self->fetchPage();
        },
        false);
}

void InatDuplicateCheck::finish(bool ok, const QString &error, int status)
{
    QHash<int, QList<DuplicateHit>> hits;
    for (const Group &g : std::as_const(m_groups)) {
        const QList<DuplicateHit> found = DuplicateFinder::find(g.observation, m_existing);
        if (!found.isEmpty())
            hits.insert(g.key, found);
    }
    m_running = false;
    emit finished(ok, error, status, hits);
}

} // namespace pl::inat
