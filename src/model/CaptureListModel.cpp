#include "model/CaptureListModel.h"

#include "db/Database.h"
#include "thumb/ThumbnailCache.h"

#include <QPainter>
#include <QPixmap>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStringList>

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
        {MatchStatusRole, "matchStatus"},
        {MatchedNameRole, "matchedName"},
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
        if (!row.matchedName.isEmpty())
            tip += QStringLiteral("\n→ %1 (%2)").arg(row.matchedName, row.matchStatus);
        else if (!row.matchStatus.isEmpty())
            tip += QStringLiteral("\n%1").arg(row.matchStatus);
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
    case MatchStatusRole:
        return row.matchStatus;
    case MatchedNameRole:
        return row.matchedName;
    default:
        return {};
    }
}

void CaptureListModel::setStatusFilter(const QString &status)
{
    if (m_statusFilter == status)
        return;
    m_statusFilter = status;
    reload();
}

void CaptureListModel::setTaxonScope(qint64 taxonInatId)
{
    if (m_taxonScope == taxonInatId)
        return;
    m_taxonScope = taxonInatId;
    reload();
}

void CaptureListModel::reload()
{
    beginResetModel();
    m_rows.clear();
    m_rowsByHash.clear();

    if (m_db.isOpen()) {
        QStringList clauses;
        if (m_statusFilter == QLatin1String("auto"))
            clauses << QStringLiteral("match_status = 'auto'");
        else if (m_statusFilter == QLatin1String("confirmed"))
            clauses << QStringLiteral("match_status = 'confirmed'");
        else if (m_statusFilter == QLatin1String("pending"))
            clauses << QStringLiteral("match_status = 'pending' AND matched_id IS NOT NULL");
        else if (m_statusFilter == QLatin1String("unmatched"))
            clauses << QStringLiteral(
                "(match_status IS NULL OR (match_status = 'pending' AND matched_id IS NULL))");

        if (m_taxonScope > 0) {
            clauses << QStringLiteral(
                "matched_id IN (SELECT id FROM taxon WHERE inat_id IN ("
                "  WITH RECURSIVE sub(x) AS (SELECT ? "
                "    UNION ALL SELECT t.inat_id FROM taxon t JOIN sub ON t.parent_inat_id = sub.x) "
                "  SELECT x FROM sub))");
        }

        const QString where =
            clauses.isEmpty() ? QString()
                              : QStringLiteral(" WHERE ") + clauses.join(QStringLiteral(" AND "));

        QSqlQuery q(QSqlDatabase::database(m_db.connectionName(), false));
        q.setForwardOnly(true);
        q.prepare(QStringLiteral(
            "SELECT id, base_name, name_text, captured_on, date_source, path, preview_path, "
            "       preview_hash, match_status, matched_name FROM ("
            "  SELECT c.id, c.base_name, c.name_text, c.captured_on, c.date_source, f.path, "
            "    (SELECT r.path FROM rendition r WHERE r.capture_id = c.id "
            "       ORDER BY (r.kind = 'raw'), r.id LIMIT 1) AS preview_path, "
            "    (SELECT r.content_hash FROM rendition r WHERE r.capture_id = c.id "
            "       ORDER BY (r.kind = 'raw'), r.id LIMIT 1) AS preview_hash, "
            "    m.status AS match_status, m.taxon_id AS matched_id, t.name AS matched_name "
            "  FROM capture c JOIN folder f ON f.id = c.folder_id "
            "  LEFT JOIN capture_match m ON m.id = ("
            "     SELECT id FROM capture_match WHERE capture_id = c.id "
            "     ORDER BY (decided_by = 'user') DESC, confidence DESC LIMIT 1) "
            "  LEFT JOIN taxon t ON t.id = m.taxon_id "
            ")") + where + QStringLiteral(
            " ORDER BY (captured_on IS NULL), captured_on DESC, id DESC"));
        if (m_taxonScope > 0)
            q.addBindValue(qlonglong(m_taxonScope));
        q.exec();

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

            const QString rawStatus = q.value(8).toString();
            const bool hasTaxon = !q.value(9).isNull();
            if (rawStatus.isEmpty())
                row.matchStatus = QStringLiteral("unmatched");
            else if (rawStatus == QLatin1String("pending") && !hasTaxon)
                row.matchStatus = QStringLiteral("unmatched");
            else
                row.matchStatus = rawStatus;
            row.matchedName = q.value(9).toString();

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
