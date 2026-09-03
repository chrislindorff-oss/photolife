#pragma once

#include "coverage/CoverageCalculator.h"
#include "taxonomy/TaxonomyTypes.h"

#include <QAbstractItemModel>
#include <QHash>
#include <QList>

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
    };

    explicit TaxonomyTreeModel(pl::Database &db, QObject *parent = nullptr);
    ~TaxonomyTreeModel() override;

    void setProject(int projectId);
    int projectId() const { return m_projectId; }

    // Coverage overlays "have / missing" and species counts onto the tree.
    void setCoverage(const pl::coverage::ProjectCoverage &coverage);

    // When on, hides every taxon with no photographed species in its subtree.
    void setPhotographedOnly(bool on);
    bool photographedOnly() const { return m_photographedOnly; }

    // The index of a taxon by its iNaturalist id, or an invalid index when the
    // taxon is not currently in the tree (e.g. filtered out). Column 0.
    QModelIndex indexForTaxon(qint64 inatId) const;

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
    };

    Node *nodeFor(const QModelIndex &index) const;
    void rebuild();
    void emitCoverageDataChanged(const QModelIndex &parent);

    pl::Database &m_db;
    int m_projectId = -1;
    QList<pl::taxonomy::TreeNode> m_flat;   // the project's taxa, parents first
    std::unique_ptr<Node> m_root;           // synthetic; its children are the real roots
    QHash<qint64, Node *> m_byId;           // live nodes by iNat id, rebuilt each rebuild()
    pl::coverage::ProjectCoverage m_coverage;
    bool m_photographedOnly = false;
};

} // namespace pl::model
