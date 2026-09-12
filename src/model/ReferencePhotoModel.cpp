#include "model/ReferencePhotoModel.h"

#include "db/Database.h"
#include "net/PhotoCache.h"
#include "taxonomy/TaxonomyStore.h"

#include <QPainter>
#include <QPixmap>

namespace pl::model {
namespace {

QIcon makePlaceholder()
{
    constexpr int px = pl::net::PhotoCache::kMaxEdge;
    QPixmap pm(px, px);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 28));
    p.drawRoundedRect(pm.rect().adjusted(24, 24, -24, -24), 8, 8);
    p.end();
    return QIcon(pm);
}

} // namespace

ReferencePhotoModel::ReferencePhotoModel(pl::Database &db, pl::net::PhotoCache &photos,
                                        QObject *parent)
    : QAbstractListModel(parent), m_db(db), m_photos(photos), m_placeholder(makePlaceholder())
{
    connect(&m_photos, &pl::net::PhotoCache::ready, this, &ReferencePhotoModel::onPhotoReady);
}

int ReferencePhotoModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_rows.size());
}

QHash<int, QByteArray> ReferencePhotoModel::roleNames() const
{
    return {
        {InatIdRole, "inatId"},       {NameRole, "name"},
        {CommonNameRole, "commonName"}, {RankRole, "rank"},
        {AttributionRole, "attribution"}, {PhotoUrlRole, "photoUrl"},
        {HasPhotoRole, "hasPhoto"},
    };
}

int ReferencePhotoModel::withPhotoCount() const
{
    int n = 0;
    for (const Row &r : m_rows) {
        if (!r.photoUrl.isEmpty())
            ++n;
    }
    return n;
}

QVariant ReferencePhotoModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size())
        return {};

    const Row &row = m_rows.at(index.row());

    switch (role) {
    case Qt::DisplayRole: {
        if (!row.commonName.isEmpty() && row.commonName != row.name)
            return row.name + QLatin1Char('\n') + row.commonName;
        return row.name;
    }
    case Qt::ToolTipRole: {
        QString tip = row.name;
        if (!row.commonName.isEmpty() && row.commonName != row.name)
            tip += QStringLiteral("\n%1").arg(row.commonName);
        tip += QStringLiteral("\n%1").arg(row.rank);
        if (row.photoUrl.isEmpty())
            tip += QStringLiteral("\n\n(no reference photo yet)");
        else if (!row.attribution.isEmpty())
            tip += QStringLiteral("\n\n%1").arg(row.attribution);
        return tip;
    }
    case Qt::DecorationRole: {
        if (row.photoUrl.isEmpty())
            return m_placeholder;
        const QPixmap pm = m_photos.photo(row.photoUrl);
        return pm.isNull() ? m_placeholder : QIcon(pm);
    }
    case InatIdRole:
        return row.inatId;
    case NameRole:
        return row.name;
    case CommonNameRole:
        return row.commonName;
    case RankRole:
        return row.rank;
    case AttributionRole:
        return row.attribution;
    case PhotoUrlRole:
        return row.photoUrl;
    case HasPhotoRole:
        return !row.photoUrl.isEmpty();
    default:
        return {};
    }
}

void ReferencePhotoModel::setProject(int projectId)
{
    if (m_projectId == projectId)
        return;
    m_projectId = projectId;
    m_scope = 0;   // a fresh tree; the tree view will re-assert any selection
    reload();
}

void ReferencePhotoModel::setScope(qint64 taxonInatId)
{
    if (m_scope == taxonInatId)
        return;
    m_scope = taxonInatId;
    reload();
}

void ReferencePhotoModel::reload()
{
    beginResetModel();
    m_rows.clear();
    m_rowsByUrl.clear();

    if (m_db.isOpen() && m_projectId > 0) {
        taxonomy::TaxonomyStore store(m_db.connectionName());
        for (const auto &lp : store.projectLeafPhotos(m_projectId, m_scope)) {
            Row row;
            row.inatId = lp.inatId;
            row.name = lp.name;
            row.commonName = lp.commonName;
            row.rank = lp.rank;
            row.photoUrl = lp.photoUrl;
            row.attribution = lp.attribution;
            if (!row.photoUrl.isEmpty())
                m_rowsByUrl[row.photoUrl].append(int(m_rows.size()));
            m_rows.append(std::move(row));
        }
    }

    endResetModel();
}

void ReferencePhotoModel::onPhotoReady(const QString &url)
{
    const auto it = m_rowsByUrl.constFind(url);
    if (it == m_rowsByUrl.constEnd())
        return;
    for (int r : *it) {
        const QModelIndex idx = index(r);
        emit dataChanged(idx, idx, {Qt::DecorationRole});
    }
}

} // namespace pl::model
