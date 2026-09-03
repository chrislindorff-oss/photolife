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

    pl::Database &m_db;
    int m_projectId = -1;
    std::unique_ptr<Node> m_root;   // synthetic; its children are the real roots
    pl::coverage::ProjectCoverage m_coverage;
};

} // namespace pl::model
