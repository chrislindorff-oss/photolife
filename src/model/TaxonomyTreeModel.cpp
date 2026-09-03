#include "model/TaxonomyTreeModel.h"

#include "db/Database.h"
#include "taxonomy/TaxonomyStore.h"

#include <QFont>

namespace pl::model {

TaxonomyTreeModel::TaxonomyTreeModel(pl::Database &db, QObject *parent)
    : QAbstractItemModel(parent), m_db(db), m_root(std::make_unique<Node>())
{
}

TaxonomyTreeModel::~TaxonomyTreeModel() = default;

void TaxonomyTreeModel::setProject(int projectId)
{
    beginResetModel();
    m_projectId = projectId;
    m_root = std::make_unique<Node>();

    if (projectId > 0 && m_db.isOpen()) {
        taxonomy::TaxonomyStore store(m_db.connectionName());
        const QList<taxonomy::TreeNode> flat = store.projectTree(projectId);

        QHash<qint64, Node *> byId;
        std::vector<std::unique_ptr<Node>> pending;
        pending.reserve(flat.size());
        for (const taxonomy::TreeNode &tn : flat) {
            auto node = std::make_unique<Node>();
            node->data = tn;
            byId.insert(tn.inatId, node.get());
            pending.push_back(std::move(node));
        }

        // projectTree() is ordered parents-before-children (by rank_level), so a
        // single pass links every node whose parent is present.
        for (auto &node : pending) {
            Node *parent = m_root.get();
            if (node->data.parentInatId) {
                if (Node *found = byId.value(*node->data.parentInatId, nullptr))
                    parent = found;
            }
            node->parent = parent;
            parent->children.push_back(std::move(node));
        }
    }

    endResetModel();
}

TaxonomyTreeModel::Node *TaxonomyTreeModel::nodeFor(const QModelIndex &index) const
{
    if (!index.isValid())
        return m_root.get();
    return static_cast<Node *>(index.internalPointer());
}

QModelIndex TaxonomyTreeModel::index(int row, int column, const QModelIndex &parent) const
{
    if (!hasIndex(row, column, parent))
        return {};
    Node *parentNode = nodeFor(parent);
    if (row < 0 || row >= int(parentNode->children.size()))
        return {};
    return createIndex(row, column, parentNode->children.at(row).get());
}

QModelIndex TaxonomyTreeModel::parent(const QModelIndex &child) const
{
    Node *node = nodeFor(child);
    if (!node || node == m_root.get() || !node->parent || node->parent == m_root.get())
        return {};

    Node *grandparent = node->parent->parent;
    if (!grandparent)
        return {};

    const auto &siblings = grandparent->children;
    for (int i = 0; i < int(siblings.size()); ++i) {
        if (siblings.at(i).get() == node->parent)
            return createIndex(i, 0, node->parent);
    }
    return {};
}

int TaxonomyTreeModel::rowCount(const QModelIndex &parent) const
{
    if (parent.column() > 0)
        return 0;
    return int(nodeFor(parent)->children.size());
}

int TaxonomyTreeModel::columnCount(const QModelIndex &) const
{
    return 2;
}

QVariant TaxonomyTreeModel::data(const QModelIndex &index, int role) const
{
    Node *node = nodeFor(index);
    if (!node || node == m_root.get())
        return {};
    const taxonomy::TreeNode &t = node->data;

    switch (role) {
    case Qt::DisplayRole:
        if (index.column() == 0) {
            if (!t.commonName.isEmpty() && t.commonName != t.name)
                return QStringLiteral("%1  ·  %2").arg(t.name, t.commonName);
            return t.name;
        }
        return t.rank;
    case Qt::FontRole:
        if (index.column() == 0 && t.isLeafRank && t.inRegion) {
            QFont f;
            f.setBold(true);
            return f;
        }
        return {};
    case Qt::ToolTipRole:
        return t.inRegion ? QObject::tr("Recorded in this region") : QVariant();
    case InatIdRole:
        return t.inatId;
    case RankRole:
        return t.rank;
    case InRegionRole:
        return t.inRegion;
    default:
        return {};
    }
}

QVariant TaxonomyTreeModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    return section == 0 ? tr("Taxon") : tr("Rank");
}

} // namespace pl::model
