#include "inat/LibraryPresence.h"

#include "taxonomy/TaxonomyStore.h"

#include <QHash>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QVariant>

namespace pl::inat {

using taxonomy::TaxonomyStore;

LibraryPresence::LibraryPresence(QString connectionName)
    : m_connectionName(std::move(connectionName))
{
}

void LibraryPresence::load()
{
    m_treeTaxa.clear();
    m_coveredTaxa.clear();
    m_unmatchedNames.clear();

    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    if (!db.isOpen())
        return;

    QSqlQuery q(db);
    q.setForwardOnly(true);
    q.exec(QStringLiteral(
        "SELECT DISTINCT t.inat_id FROM project_taxon pt JOIN taxon t ON t.id = pt.taxon_id"));
    while (q.next())
        m_treeTaxa.insert(q.value(0).toLongLong());

    QHash<qint64, qint64> parentOf;
    q.exec(QStringLiteral("SELECT inat_id, parent_inat_id FROM taxon WHERE parent_inat_id IS NOT NULL"));
    while (q.next())
        parentOf.insert(q.value(0).toLongLong(), q.value(1).toLongLong());

    // Each capture's top match, the same ranking CaptureListModel uses.
    q.exec(QStringLiteral(
        "SELECT c.name_text, m.status, t.inat_id FROM capture c "
        "LEFT JOIN capture_match m ON m.id = ("
        "  SELECT id FROM capture_match WHERE capture_id = c.id "
        "  ORDER BY (decided_by = 'user') DESC, confidence DESC LIMIT 1) "
        "LEFT JOIN taxon t ON t.id = m.taxon_id"));
    while (q.next()) {
        const QString status = q.value(1).toString();
        const bool matched = !q.value(2).isNull()
                             && (status == QLatin1String("auto")
                                 || status == QLatin1String("confirmed"));
        if (!matched) {
            const QString folded = TaxonomyStore::foldName(q.value(0).toString());
            if (!folded.isEmpty())
                m_unmatchedNames.insert(folded);
            continue;
        }
        // The matched taxon and every ancestor of it (stopping at one already
        // covered -- its ancestors are then covered too).
        for (qint64 id = q.value(2).toLongLong(); id > 0 && !m_coveredTaxa.contains(id);
             id = parentOf.value(id, 0))
            m_coveredTaxa.insert(id);
    }
}

bool LibraryPresence::inAnyTree(qint64 taxonInatId) const
{
    return m_treeTaxa.contains(taxonInatId);
}

bool LibraryPresence::inLibrary(qint64 taxonInatId, const QString &name,
                                const QString &commonName) const
{
    if (taxonInatId > 0 && m_coveredTaxa.contains(taxonInatId))
        return true;
    for (const QString &n : {name, commonName}) {
        const QString folded = TaxonomyStore::foldName(n);
        if (!folded.isEmpty() && m_unmatchedNames.contains(folded))
            return true;
    }
    return false;
}

void LibraryPresence::annotate(const QString &connectionName, QList<Candidate> &candidates)
{
    LibraryPresence presence(connectionName);
    presence.load();
    for (Candidate &c : candidates) {
        const taxonomy::Observation &o = c.observation;
        c.presenceChecked = true;
        c.inAnyTree = presence.inAnyTree(o.taxonInatId);
        c.inLibrary = presence.inLibrary(o.taxonInatId, o.taxonName, o.taxonCommonName);
    }
}

} // namespace pl::inat
