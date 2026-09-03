#include "model/TaxonomyTreeModel.h"

#include "db/Database.h"
#include "taxonomy/TaxonomyStore.h"

#include <QColor>
#include <QFont>
#include <QStringList>

namespace pl::model {

TaxonomyTreeModel::TaxonomyTreeModel(pl::Database &db, QObject *parent)
    : QAbstractItemModel(parent), m_db(db), m_root(std::make_unique<Node>())
{
}

TaxonomyTreeModel::~TaxonomyTreeModel() = default;

void TaxonomyTreeModel::setProject(int projectId)
{
    m_projectId = projectId;
    m_flat.clear();
    if (projectId > 0 && m_db.isOpen()) {
        taxonomy::TaxonomyStore store(m_db.connectionName());
        m_flat = store.projectTree(projectId);
    }
    rebuild();
}

void TaxonomyTreeModel::setCoverage(const pl::coverage::ProjectCoverage &coverage)
{
    m_coverage = coverage;
    if (m_photographedOnly) {
        // The visible set depends on coverage, so the tree structure changes.
        rebuild();
        return;
    }
    // Structure is unchanged; coverage feeds several roles on every node.
    emitCoverageDataChanged({});
}

void TaxonomyTreeModel::emitCoverageDataChanged(const QModelIndex &parent)
{
    const int n = rowCount(parent);
    if (n == 0)
        return;
    emit dataChanged(index(0, 0, parent), index(n - 1, columnCount() - 1, parent),
                     {Qt::DisplayRole, Qt::FontRole, Qt::ForegroundRole, Qt::ToolTipRole,
                      HasPhotosRole, SpeciesTotalRole, SpeciesWithPhotosRole, StatusRole});
    for (int r = 0; r < n; ++r)
        emitCoverageDataChanged(index(r, 0, parent));
}

void TaxonomyTreeModel::setPhotographedOnly(bool on)
{
    if (m_photographedOnly == on)
        return;
    m_photographedOnly = on;
    rebuild();
}

void TaxonomyTreeModel::rebuild()
{
    beginResetModel();
    m_root = std::make_unique<Node>();
    m_byId.clear();   // every Node is reallocated below

    // In "photographed only" mode a taxon is kept when its subtree has a photo.
    // subtreeHasPhotos rolls up, so a kept node's ancestors are always kept too
    // and no child is ever orphaned.
    auto keep = [this](qint64 inatId) {
        if (!m_photographedOnly)
            return true;
        return m_coverage.byTaxon.value(inatId).subtreeHasPhotos;
    };

    std::vector<std::unique_ptr<Node>> pending;
    pending.reserve(m_flat.size());
    for (const taxonomy::TreeNode &tn : m_flat) {
        if (!keep(tn.inatId))
            continue;
        auto node = std::make_unique<Node>();
        node->data = tn;
        m_byId.insert(tn.inatId, node.get());
        pending.push_back(std::move(node));
    }

    // m_flat is ordered parents-before-children (by rank_level).
    for (auto &node : pending) {
        Node *parent = m_root.get();
        if (node->data.parentInatId) {
            if (Node *found = m_byId.value(*node->data.parentInatId, nullptr))
                parent = found;
        }
        node->parent = parent;
        parent->children.push_back(std::move(node));
    }

    endResetModel();
}

QModelIndex TaxonomyTreeModel::indexForTaxon(qint64 inatId) const
{
    Node *node = m_byId.value(inatId, nullptr);
    if (!node || !node->parent)
        return {};
    const auto &siblings = node->parent->children;
    for (int i = 0; i < int(siblings.size()); ++i) {
        if (siblings.at(i).get() == node)
            return createIndex(i, 0, node);
    }
    return {};
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
    return 3;
}

QVariant TaxonomyTreeModel::data(const QModelIndex &index, int role) const
{
    Node *node = nodeFor(index);
    if (!node || node == m_root.get())
        return {};
    const taxonomy::TreeNode &t = node->data;

    switch (role) {
    case Qt::DisplayRole: {
        if (index.column() == 0) {
            if (!t.commonName.isEmpty() && t.commonName != t.name)
                return QStringLiteral("%1  ·  %2").arg(t.name, t.commonName);
            return t.name;
        }
        if (index.column() == 1)
            return t.rank;

        // Column 2: coverage.
        const auto cov = m_coverage.byTaxon.constFind(t.inatId);
        if (cov == m_coverage.byTaxon.constEnd())
            return {};
        if (t.isLeafRank)
            return cov->subtreeHasPhotos ? QStringLiteral("✔") : QStringLiteral("·");
        if (cov->speciesTotal > 0)
            return QStringLiteral("%1 / %2").arg(cov->speciesWithPhotos).arg(cov->speciesTotal);
        return {};
    }
    case Qt::TextAlignmentRole:
        return index.column() == 2 ? int(Qt::AlignCenter) : QVariant();
    case Qt::FontRole:
        if (index.column() == 0 && t.isLeafRank && t.inRegion) {
            QFont f;
            f.setBold(true);
            return f;
        }
        return {};
    case Qt::ForegroundRole: {
        if (index.column() != 2 || !t.isLeafRank)
            return {};
        const auto cov = m_coverage.byTaxon.constFind(t.inatId);
        if (cov == m_coverage.byTaxon.constEnd())
            return {};
        return cov->subtreeHasPhotos ? QColor(0x2E, 0x7D, 0x32) : QColor(0xB0, 0xB0, 0xB0);
    }
    case Qt::ToolTipRole: {
        QStringList bits;
        if (t.inRegion)
            bits << tr("Recorded in this region");
        if (const auto cov = m_coverage.byTaxon.constFind(t.inatId);
            cov != m_coverage.byTaxon.constEnd() && !cov->status.isEmpty())
            bits << cov->status;
        return bits.isEmpty() ? QVariant() : bits.join(QStringLiteral(" · "));
    }
    case InatIdRole:
        return t.inatId;
    case RankRole:
        return t.rank;
    case InRegionRole:
        return t.inRegion;
    case SpeciesTotalRole:
        return m_coverage.byTaxon.value(t.inatId).speciesTotal;
    case SpeciesWithPhotosRole:
        return m_coverage.byTaxon.value(t.inatId).speciesWithPhotos;
    case HasPhotosRole:
        return m_coverage.byTaxon.value(t.inatId).subtreeHasPhotos;
    case StatusRole:
        return m_coverage.byTaxon.value(t.inatId).status;
    default:
        return {};
    }
}

QVariant TaxonomyTreeModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (section) {
    case 0:  return tr("Taxon");
    case 1:  return tr("Rank");
    default: return tr("Coverage");
    }
}

} // namespace pl::model
