#pragma once

#include "taxonomy/TaxonomyStore.h"
#include "taxonomy/TaxonomyTypes.h"

#include <QList>
#include <QObject>
#include <QSet>

namespace pl::net {
class INatClient;
}

namespace pl::taxonomy {

// One thing a refresh found changed about a tree's taxon on iNaturalist.
struct TreeChange
{
    enum Kind
    {
        Renamed,       // inactive, with one current name (a swap or merge)
        Split,         // inactive, split into several
        Retired,       // inactive, with no replacement
        NotRecorded,   // a species iNaturalist no longer lists in the tree's place
        SubspeciesNotRecorded,   // below species, with no records named that far there
        EmptyGroup,    // a genus, family... with nothing beneath it in the tree
    };
    Kind kind = NotRecorded;
    TaxonomyStore::ChangeCandidate taxon;
    QList<Taxon> replacements;   // Renamed: one; Split: several
};

// After a complete refresh (ProjectBuilder::listWasComplete()), works out
// what's gone from a tree: its species the species list no longer gave, and
// groups left with nothing beneath. Asks iNaturalist about each -- an
// inactive one has been renamed, split or retired -- and reports them for
// the user to decide; nothing is changed here. Taxa the user kept
// (TaxonomyStore::keepProjectTaxa) aren't reported again, unless renamed.
// A species from a checklist is only ever reported when it's inactive:
// iNaturalist's list was never where it came from. Subspecies are reported
// apart: most observations are named only to species, so one with no records
// of its own in the place may well still live there.
class TreeChangeFinder : public QObject
{
    Q_OBJECT

public:
    TreeChangeFinder(net::INatClient &inat, TaxonomyStore &store, QObject *parent = nullptr);

    void find(int projectId, const QSet<qint64> &listedTaxa);
    bool isRunning() const { return m_running; }

    // Grouped by kind, then by name.
    static void sort(QList<TreeChange> &changes);

signals:
    void finished(int projectId, const QList<pl::taxonomy::TreeChange> &changes, const QString &error);

private:
    void fetchNext();
    void fetchReplacements();
    void classify();
    void done(const QString &error);

    static constexpr int kBatch = 30;   // iNaturalist's /taxa/<ids> limit

    net::INatClient &m_inat;
    TaxonomyStore &m_store;
    bool m_running = false;
    int m_projectId = 0;
    QList<TaxonomyStore::ChangeCandidate> m_candidates;
    QList<qint64> m_toFetch;
    QHash<qint64, Taxon> m_fetched;   // by inat id: the candidates, then their replacements
    bool m_fetchingReplacements = false;
};

} // namespace pl::taxonomy
