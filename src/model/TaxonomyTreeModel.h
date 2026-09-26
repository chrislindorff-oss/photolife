#pragma once

#include "coverage/CoverageCalculator.h"
#include "taxonomy/TaxonomyTypes.h"

#include <QAbstractItemModel>
#include <QHash>
#include <QList>
#include <QSet>
#include <QString>

#include <memory>

namespace pl {
class Database;
}

namespace pl::model {

// Read-only tree over a project's cached taxonomy (TaxonomyStore::projectTree).
// Columns: 0 = name, 1 = rank. Species in the project's region are flagged.
class TaxonomyTreeModel : public QAbstractItemModel
{
    Q_OBJECT

public:
    enum Roles {
        InatIdRole = Qt::UserRole + 1,
        RankRole,
        InRegionRole,
        SpeciesTotalRole,
        SpeciesWithPhotosRole,
        HasPhotosRole,
        StatusRole,
        NameRole,       // accepted scientific name only (no common-name suffix)
        ContextOnlyRole,   // shown only as the path to a coverage-filter match
    };

    // Filters the visible-rank taxa by whether their coverage is complete:
    // every species below photographed ("X / X"), or, for a species or
    // lower-rank taxon with no species beneath it, the taxon photographed.
    enum class CoverageFilter { All, Complete, Incomplete };

    explicit TaxonomyTreeModel(pl::Database &db, QObject *parent = nullptr);
    ~TaxonomyTreeModel() override;

    void setProject(int projectId);
    int projectId() const { return m_projectId; }

    // Coverage overlays "have / missing" and species counts onto the tree.
    void setCoverage(const pl::coverage::ProjectCoverage &coverage);

    // When on, hides every taxon with no photographed species in its subtree.
    void setPhotographedOnly(bool on);
    bool photographedOnly() const { return m_photographedOnly; }

    // When on, hides every taxon with no non-empty conservation status
    // anywhere in its subtree (any tier).
    void setThreatenedOnly(bool on);
    bool threatenedOnly() const { return m_threatenedOnly; }

    // When on, hides every taxon with no introduced species in its subtree,
    // and marks the introduced species (blue, "*" in the Coverage column).
    void setIntroducedOnly(bool on);
    bool introducedOnly() const { return m_introducedOnly; }
    // iNat ids of the species introduced in the tree's region, ignoring
    // active filters (species only, like ProjectCoverage::introducedTotal).
    QList<qint64> introducedTaxa() const;

    // "" = off. Otherwise hides every taxon with no species carrying exactly
    // this status string anywhere in its subtree.
    void setStatusFilter(const QString &status);
    QString statusFilter() const { return m_statusFilter; }

    // Keeps only visible-rank taxa matching the filter (see CoverageFilter),
    // plus the ancestors needed to reach them. Those ancestors don't match
    // themselves -- a complete genus in an incomplete family -- so they are
    // flagged ContextOnlyRole and drawn muted. Taxa of hidden ranks never
    // count as matches.
    void setCoverageFilter(CoverageFilter filter);
    CoverageFilter coverageFilter() const { return m_coverageFilter; }

    // iNat ids of species and lower taxa (subspecies, varieties ... --
    // matching how ProjectCoverage::byStatus/threatenedTotal are tallied)
    // currently carrying this exact status, ignoring active filters -- scans
    // the whole project like findTaxa() does.
    QList<qint64> taxaWithStatus(const QString &status) const;

    // Same, but any non-empty status (species and lower, like threatenedTotal).
    QList<qint64> anyThreatenedTaxa() const;

    // The distinct ranks present in the current project's tree, coarsest first
    // (kingdom, phylum, ... species, subspecies, ...).
    QStringList availableRanks() const;

    // Ranks in this set are hidden: their taxa are removed from the tree and
    // their children are reparented to the nearest visible ancestor, so hiding
    // e.g. "genus" moves species up to sit directly under family.
    void setHiddenRanks(const QSet<QString> &ranks);
    QSet<QString> hiddenRanks() const { return m_hiddenRanks; }

    // The index of a taxon by its iNaturalist id, or an invalid index when the
    // taxon is not currently in the tree (e.g. filtered out). Column 0.
    QModelIndex indexForTaxon(qint64 inatId) const;

    // iNat ids of every taxon currently materialized in the tree -- i.e. what
    // survived the active filters (photographedOnly/threatenedOnly/statusFilter)
    // and rank-hiding, exactly what's visible on screen right now. For a caller
    // that wants to mirror "what's on screen" elsewhere (the photo export
    // dialog's "match what's currently shown" option).
    QList<qint64> visibleTaxonIds() const { return m_byId.keys(); }

    // iNat ids of taxa whose scientific or common name matches `text` (folded,
    // case-insensitive), best matches first: a prefix on the scientific name,
    // then a prefix on the common name, then any substring. Searches the whole
    // project — including taxa the "photographed only" filter is hiding.
    QList<qint64> findTaxa(const QString &text, int limit = 50) const;

    QModelIndex index(int row, int column, const QModelIndex &parent = {}) const override;
    QModelIndex parent(const QModelIndex &child) const override;
    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

private:
    struct Node
    {
        pl::taxonomy::TreeNode data;
        Node *parent = nullptr;
        std::vector<std::unique_ptr<Node>> children;
        bool contextOnly = false;
    };

    Node *nodeFor(const QModelIndex &index) const;
    void rebuild();
    void emitCoverageDataChanged(const QModelIndex &parent);
    bool isCoverageComplete(qint64 inatId) const;
    // A taxon the active threatened filter is about: any listed taxon under
    // "threatened only", or one with exactly the filtered status.
    bool matchesStatusFilter(const pl::coverage::TaxonCoverage &cov) const;
    // An introduced species while the introduced filter is on.
    bool matchesIntroducedFilter(const pl::taxonomy::TreeNode &t,
                                 const pl::coverage::TaxonCoverage &cov) const;

    pl::Database &m_db;
    int m_projectId = -1;
    QList<pl::taxonomy::TreeNode> m_flat;   // the project's taxa, parents first
    std::unique_ptr<Node> m_root;           // synthetic; its children are the real roots
    QHash<qint64, Node *> m_byId;           // live nodes by iNat id, rebuilt each rebuild()
    pl::coverage::ProjectCoverage m_coverage;
    bool m_photographedOnly = false;
    bool m_threatenedOnly = false;
    bool m_introducedOnly = false;
    QString m_statusFilter;
    CoverageFilter m_coverageFilter = CoverageFilter::All;
    QSet<QString> m_hiddenRanks;
};

} // namespace pl::model
