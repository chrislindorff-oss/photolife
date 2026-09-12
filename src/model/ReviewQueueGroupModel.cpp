#include "model/ReviewQueueGroupModel.h"

namespace pl::model {
namespace {
// internalId of a top-level row (group header or lone capture). Child rows carry
// their parent group's index instead, which is always a small non-negative int.
constexpr quintptr kTop = ~quintptr(0);
} // namespace

ReviewQueueGroupModel::ReviewQueueGroupModel(QObject *parent)
    : QAbstractItemModel(parent)
{
}

void ReviewQueueGroupModel::setSourceModel(QAbstractItemModel *source)
{
    if (m_source == source)
        return;

    beginResetModel();
    if (m_source)
        m_source->disconnect(this);
    m_source = source;
    if (m_source) {
        const auto structural = [this] {
            beginResetModel();
            rebuild();
            endResetModel();
        };
        connect(m_source, &QAbstractItemModel::modelReset, this, structural);
        connect(m_source, &QAbstractItemModel::rowsInserted, this, structural);
        connect(m_source, &QAbstractItemModel::rowsRemoved, this, structural);
        connect(m_source, &QAbstractItemModel::rowsMoved, this, structural);
        connect(m_source, &QAbstractItemModel::layoutChanged, this, structural);
        connect(m_source, &QAbstractItemModel::dataChanged, this,
                &ReviewQueueGroupModel::onSourceDataChanged);
    }
    rebuild();
    endResetModel();
}

void ReviewQueueGroupModel::rebuild()
{
    m_groups.clear();
    if (!m_source)
        return;

    QHash<QString, int> byName;
    const int rows = m_source->rowCount();
    for (int r = 0; r < rows; ++r) {
        const QString name = m_source->index(r, 0).data(Qt::DisplayRole).toString();
        const auto it = byName.constFind(name);
        if (it == byName.constEnd()) {
            byName.insert(name, int(m_groups.size()));
            m_groups.append({name, QList<int>{r}});
        } else {
            m_groups[*it].sourceRows.append(r);
        }
    }
}

QModelIndex ReviewQueueGroupModel::index(int row, int column, const QModelIndex &parent) const
{
    if (column != 0 || row < 0)
        return {};

    if (!parent.isValid())
        return row < m_groups.size() ? createIndex(row, column, kTop) : QModelIndex();

    if (parent.internalId() != kTop)
        return {};   // capture leaves never have children
    const int gi = parent.row();
    if (gi < 0 || gi >= m_groups.size() || m_groups[gi].sourceRows.size() <= 1)
        return {};
    if (row >= m_groups[gi].sourceRows.size())
        return {};
    return createIndex(row, column, quintptr(gi));
}

QModelIndex ReviewQueueGroupModel::parent(const QModelIndex &child) const
{
    if (!child.isValid())
        return {};
    const quintptr id = child.internalId();
    if (id == kTop)
        return {};
    return createIndex(int(id), 0, kTop);
}

int ReviewQueueGroupModel::rowCount(const QModelIndex &parent) const
{
    if (!parent.isValid())
        return int(m_groups.size());
    if (parent.internalId() != kTop)
        return 0;
    const int gi = parent.row();
    if (gi < 0 || gi >= m_groups.size())
        return 0;
    const int n = m_groups[gi].sourceRows.size();
    return n > 1 ? n : 0;
}

int ReviewQueueGroupModel::columnCount(const QModelIndex &) const
{
    return 1;
}

QVariant ReviewQueueGroupModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || !m_source)
        return {};

    const quintptr id = index.internalId();
    if (id == kTop) {
        const int gi = index.row();
        if (gi < 0 || gi >= m_groups.size())
            return {};
        const Group &g = m_groups[gi];
        if (g.sourceRows.size() == 1)
            return m_source->index(g.sourceRows.first(), 0).data(role);

        // A group header is just the shared name and a count — deliberately no
        // thumbnail; expanding it reveals the photos.
        switch (role) {
        case Qt::DisplayRole:
            return QStringLiteral("%1  (%2)")
                .arg(g.name.isEmpty() ? tr("Unnamed") : g.name)
                .arg(g.sourceRows.size());
        case Qt::ToolTipRole:
            return tr("%n photo(s) to review under this name", nullptr, g.sourceRows.size());
        default:
            return {};
        }
    }

    const int gi = int(id);
    if (gi < 0 || gi >= m_groups.size())
        return {};
    const Group &g = m_groups[gi];
    if (index.row() < 0 || index.row() >= g.sourceRows.size())
        return {};
    return m_source->index(g.sourceRows[index.row()], 0).data(role);
}

Qt::ItemFlags ReviewQueueGroupModel::flags(const QModelIndex &index) const
{
    if (!index.isValid())
        return Qt::NoItemFlags;
    if (isGroup(index))
        return Qt::ItemIsEnabled;   // can expand/collapse, but selection lands on a capture
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
}

QHash<int, QByteArray> ReviewQueueGroupModel::roleNames() const
{
    return m_source ? m_source->roleNames() : QAbstractItemModel::roleNames();
}

bool ReviewQueueGroupModel::isGroup(const QModelIndex &index) const
{
    if (!index.isValid() || index.internalId() != kTop)
        return false;
    const int gi = index.row();
    return gi >= 0 && gi < m_groups.size() && m_groups[gi].sourceRows.size() > 1;
}

QString ReviewQueueGroupModel::groupName(const QModelIndex &index) const
{
    if (!index.isValid())
        return {};
    const quintptr id = index.internalId();
    const int gi = (id == kTop) ? index.row() : int(id);
    return (gi >= 0 && gi < m_groups.size()) ? m_groups[gi].name : QString();
}

int ReviewQueueGroupModel::captureCount() const
{
    int n = 0;
    for (const Group &g : m_groups)
        n += g.sourceRows.size();
    return n;
}

QModelIndex ReviewQueueGroupModel::captureAt(int n) const
{
    if (n < 0)
        return {};
    int seen = 0;
    for (int gi = 0; gi < m_groups.size(); ++gi) {
        const int sz = m_groups[gi].sourceRows.size();
        if (n < seen + sz) {
            const int local = n - seen;
            return sz == 1 ? createIndex(gi, 0, kTop) : createIndex(local, 0, quintptr(gi));
        }
        seen += sz;
    }
    return {};
}

int ReviewQueueGroupModel::captureNumberOf(const QModelIndex &index) const
{
    if (!index.isValid())
        return -1;
    const quintptr id = index.internalId();
    int seen = 0;
    if (id == kTop) {
        for (int gi = 0; gi < m_groups.size(); ++gi) {
            if (gi == index.row())
                return m_groups[gi].sourceRows.size() == 1 ? seen : -1;
            seen += m_groups[gi].sourceRows.size();
        }
        return -1;
    }
    const int target = int(id);
    for (int gi = 0; gi < m_groups.size(); ++gi) {
        if (gi == target)
            return seen + index.row();
        seen += m_groups[gi].sourceRows.size();
    }
    return -1;
}

QModelIndex ReviewQueueGroupModel::groupParentOf(const QModelIndex &index) const
{
    if (!index.isValid() || index.internalId() == kTop)
        return {};
    return createIndex(int(index.internalId()), 0, kTop);
}

void ReviewQueueGroupModel::onSourceDataChanged(const QModelIndex &topLeft,
                                                const QModelIndex &bottomRight,
                                                const QList<int> &roles)
{
    if (!topLeft.isValid() || !bottomRight.isValid())
        return;
    const int lo = topLeft.row();
    const int hi = bottomRight.row();

    for (int gi = 0; gi < m_groups.size(); ++gi) {
        const Group &g = m_groups[gi];
        const bool grouped = g.sourceRows.size() > 1;
        for (int ci = 0; ci < g.sourceRows.size(); ++ci) {
            const int sr = g.sourceRows[ci];
            if (sr < lo || sr > hi)
                continue;
            if (grouped) {
                // The header shows only a static name + count, so leaf updates
                // (thumbnails arriving, etc.) never need to reach it.
                const QModelIndex leaf = createIndex(ci, 0, quintptr(gi));
                emit dataChanged(leaf, leaf, roles);
            } else {
                const QModelIndex lone = createIndex(gi, 0, kTop);
                emit dataChanged(lone, lone, roles);
            }
        }
    }
}

} // namespace pl::model
