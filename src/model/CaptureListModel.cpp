#include "model/CaptureListModel.h"

#include "db/Database.h"
#include "thumb/ThumbnailCache.h"

#include <QPainter>
#include <QPixmap>
#include <QSqlDatabase>
#include <QSqlQuery>

namespace pl::model {
namespace {

constexpr int kThumbPx = pl::thumb::ThumbnailCache::kGridPx;

QIcon makePlaceholder()
{
    QPixmap pm(kThumbPx, kThumbPx);
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

CaptureListModel::CaptureListModel(pl::Database &db, pl::thumb::ThumbnailCache &thumbs,
                                   QObject *parent)
    : QAbstractListModel(parent), m_db(db), m_thumbs(thumbs), m_placeholder(makePlaceholder())
{
    connect(&m_thumbs, &pl::thumb::ThumbnailCache::ready,
            this, &CaptureListModel::onThumbnailReady);
}

int CaptureListModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_rows.size());
}

QHash<int, QByteArray> CaptureListModel::roleNames() const
{
    return {
        {IdRole, "captureId"},
        {NameRole, "name"},
        {FolderPathRole, "folderPath"},
        {CapturedOnRole, "capturedOn"},
        {DateSourceRole, "dateSource"},
        {PreviewPathRole, "previewPath"},
    };
}

QVariant CaptureListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size())
        return {};

    const Row &row = m_rows.at(index.row());

    switch (role) {
    case Qt::DisplayRole:
        return row.name.isEmpty() ? row.baseName : row.name;
    case Qt::ToolTipRole: {
        QString tip = row.folderPath;
        if (!row.capturedOn.isEmpty())
            tip += QStringLiteral("\n%1 (%2)").arg(row.capturedOn, row.dateSource);
        return tip;
    }
    case Qt::DecorationRole: {
        if (row.previewPath.isEmpty())
            return m_placeholder;
        const QPixmap pm = m_thumbs.thumbnail(row.previewHash, row.previewPath, kThumbPx);
        return pm.isNull() ? m_placeholder : QIcon(pm);
    }
    case IdRole:
        return row.id;
    case NameRole:
        return row.name;
    case FolderPathRole:
        return row.folderPath;
    case CapturedOnRole:
        return row.capturedOn;
    case DateSourceRole:
        return row.dateSource;
    case PreviewPathRole:
        return row.previewPath;
    default:
        return {};
    }
}

void CaptureListModel::reload()
{
    beginResetModel();
    m_rows.clear();
    m_rowsByHash.clear();

    if (m_db.isOpen()) {
        QSqlQuery q(QSqlDatabase::database(m_db.connectionName(), false));
        q.setForwardOnly(true);
        q.exec(QStringLiteral(
            "SELECT c.id, c.base_name, c.name_text, c.captured_on, c.date_source, f.path, "
            "  (SELECT r.path FROM rendition r WHERE r.capture_id = c.id "
            "     ORDER BY (r.kind = 'raw'), r.id LIMIT 1), "
            "  (SELECT r.content_hash FROM rendition r WHERE r.capture_id = c.id "
            "     ORDER BY (r.kind = 'raw'), r.id LIMIT 1) "
            "FROM capture c JOIN folder f ON f.id = c.folder_id "
            "ORDER BY (c.captured_on IS NULL), c.captured_on DESC, c.id DESC"));

        while (q.next()) {
            Row row;
            row.id = q.value(0).toInt();
            row.baseName = q.value(1).toString();
            row.name = q.value(2).toString();
            row.capturedOn = q.value(3).toString();
            row.dateSource = q.value(4).toString();
            row.folderPath = q.value(5).toString();
            row.previewPath = q.value(6).toString();
            row.previewHash = q.value(7).toString();
            if (!row.previewHash.isEmpty())
                m_rowsByHash[row.previewHash].append(int(m_rows.size()));
            m_rows.append(std::move(row));
        }
    }

    endResetModel();
}

void CaptureListModel::onThumbnailReady(const QString &contentHash, int longestEdge)
{
    if (longestEdge != kThumbPx)
        return;
    const auto it = m_rowsByHash.constFind(contentHash);
    if (it == m_rowsByHash.constEnd())
        return;
    for (int r : *it) {
        const QModelIndex idx = index(r);
        emit dataChanged(idx, idx, {Qt::DecorationRole});
    }
}

} // namespace pl::model
