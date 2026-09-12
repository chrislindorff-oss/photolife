#include "model/CaptureListModel.h"

#include "db/Database.h"
#include "thumb/ThumbnailCache.h"

#include <QPainter>
#include <QPixmap>
#include <QSet>
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
        {DisplayNameRole, "displayName"},
        {ExtRole, "ext"},
        {HasGpsRole, "hasGps"},
        {LatitudeRole, "latitude"},
        {LongitudeRole, "longitude"},
        {IsBestShotRole, "isBestShot"},
        {ShowFileTypeBadgeRole, "showFileTypeBadge"},
        {FullCaptionRole, "fullCaption"},
        {LocalityRole, "locality"},
    };
}

QVariant CaptureListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size())
        return {};

    const Row &row = m_rows.at(index.row());
    const QString displayName = row.name.isEmpty() ? row.baseName : row.name;

    // Shared field composition: `includeFileType` is false for the grid
    // caption (file type is drawn as an on-image badge there instead) and
    // true for the full-size viewer, which has room for it as plain text.
    auto compose = [&](bool includeFileType) {
        QStringList lines;
        if ((m_captionFields & CaptionName) && !displayName.isEmpty())
            lines << displayName;
        if ((m_captionFields & CaptionTaxon) && !row.matchedName.isEmpty())
            lines << row.matchedName;
        if ((m_captionFields & CaptionDate) && !row.capturedOn.isEmpty())
            lines << row.capturedOn.left(10);
        if ((m_captionFields & CaptionFilename) && !row.baseName.isEmpty())
            lines << row.baseName;
        if ((m_captionFields & CaptionLocality) && !row.locality.isEmpty())
            lines << row.locality;
        if (includeFileType && (m_captionFields & CaptionFileType) && !row.ext.isEmpty())
            lines << row.ext.toUpper();
        return lines.join(QLatin1Char('\n'));
    };

    switch (role) {
    case Qt::DisplayRole:
        return compose(false);
    case FullCaptionRole:
        return compose(true);
    case Qt::ToolTipRole: {
        QString tip = row.folderPath;
        if (!row.capturedOn.isEmpty())
            tip += QStringLiteral("\n%1 (%2)").arg(row.capturedOn, row.dateSource);
        if (!row.matchedName.isEmpty())
            tip += QStringLiteral("\n→ %1 (%2)").arg(row.matchedName, row.matchStatus);
        else if (!row.matchStatus.isEmpty())
            tip += QStringLiteral("\n%1").arg(row.matchStatus);
        tip += row.hasGps ? QStringLiteral("\nGPS: %1, %2")
                                .arg(row.latitude, 0, 'f', 5)
                                .arg(row.longitude, 0, 'f', 5)
                          : QStringLiteral("\nGPS: no");
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
    case DisplayNameRole:
        return displayName;
    case ExtRole:
        return row.ext;
    case HasGpsRole:
        return row.hasGps;
    case LatitudeRole:
        return row.latitude;
    case LongitudeRole:
        return row.longitude;
    case IsBestShotRole:
        return row.isBestShot;
    case ShowFileTypeBadgeRole:
        return (m_captionFields & CaptionFileType) != 0;
    case LocalityRole:
        return row.locality;
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

void CaptureListModel::setProjectScope(int projectId)
{
    const int normalised = projectId > 0 ? projectId : 0;
    if (m_projectScope == normalised)
        return;
    m_projectScope = normalised;
    reload();
}

void CaptureListModel::setBestShotOnly(bool on)
{
    if (m_bestShotOnly == on)
        return;
    m_bestShotOnly = on;
    reload();
}

void CaptureListModel::applyBestShot(const QList<int> &captureIds, bool on)
{
    if (captureIds.isEmpty())
        return;
    const QSet<int> ids(captureIds.cbegin(), captureIds.cend());
    for (int i = 0; i < m_rows.size(); ++i) {
        if (m_rows[i].isBestShot == on || !ids.contains(m_rows[i].id))
            continue;
        m_rows[i].isBestShot = on;
        const QModelIndex idx = index(i);
        emit dataChanged(idx, idx, {IsBestShotRole});
    }
}

void CaptureListModel::setCaptionFields(int fields)
{
    if (m_captionFields == fields)
        return;
    m_captionFields = fields;
    if (!m_rows.isEmpty())
        emit dataChanged(index(0), index(int(m_rows.size()) - 1), {Qt::DisplayRole});
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

        if (m_projectScope > 0 && m_taxonScope > 0) {
            // Descendants of the selected taxon, then narrowed to taxa that are
            // actually in this reference tree — otherwise selecting an ancestor
            // node a tree only shows for structure (up to the synthetic "Life"
            // root) would pull in every other tree's photos under it.
            clauses << QStringLiteral(
                "matched_id IN ("
                "  SELECT pt.taxon_id FROM project_taxon pt JOIN taxon pt_t ON pt_t.id = pt.taxon_id "
                "  WHERE pt.project_id = ? AND pt_t.inat_id IN ("
                "    WITH RECURSIVE sub(x) AS (SELECT ? "
                "      UNION ALL SELECT t.inat_id FROM taxon t JOIN sub ON t.parent_inat_id = sub.x) "
                "    SELECT x FROM sub))");
        } else if (m_projectScope > 0) {
            // No taxon picked: still confine to this reference tree rather than
            // falling back to the whole library.
            clauses << QStringLiteral(
                "matched_id IN (SELECT taxon_id FROM project_taxon WHERE project_id = ?)");
        } else if (m_taxonScope > 0) {
            clauses << QStringLiteral(
                "matched_id IN (SELECT id FROM taxon WHERE inat_id IN ("
                "  WITH RECURSIVE sub(x) AS (SELECT ? "
                "    UNION ALL SELECT t.inat_id FROM taxon t JOIN sub ON t.parent_inat_id = sub.x) "
                "  SELECT x FROM sub))");
        }

        if (m_bestShotOnly)
            clauses << QStringLiteral("is_best_shot = 1");

        const QString where =
            clauses.isEmpty() ? QString()
                              : QStringLiteral(" WHERE ") + clauses.join(QStringLiteral(" AND "));

        QSqlQuery q(QSqlDatabase::database(m_db.connectionName(), false));
        q.setForwardOnly(true);
        q.prepare(QStringLiteral(
            "SELECT id, base_name, name_text, captured_on, date_source, path, preview_path, "
            "       preview_hash, match_status, matched_name, preview_ext, latitude, longitude, "
            "       is_best_shot, locality "
            "FROM ("
            "  SELECT c.id, c.base_name, c.name_text, c.captured_on, c.date_source, f.path, "
            "    (SELECT r.path FROM rendition r WHERE r.capture_id = c.id "
            "       ORDER BY (r.kind = 'raw'), r.id LIMIT 1) AS preview_path, "
            "    (SELECT r.content_hash FROM rendition r WHERE r.capture_id = c.id "
            "       ORDER BY (r.kind = 'raw'), r.id LIMIT 1) AS preview_hash, "
            "    (SELECT r.ext FROM rendition r WHERE r.capture_id = c.id "
            "       ORDER BY (r.kind = 'raw'), r.id LIMIT 1) AS preview_ext, "
            "    m.status AS match_status, m.taxon_id AS matched_id, t.name AS matched_name, "
            "    c.latitude, c.longitude, "
            "    EXISTS(SELECT 1 FROM best_shot bs WHERE bs.capture_id = c.id) AS is_best_shot, "
            "    g.locality AS locality "
            "  FROM capture c JOIN folder f ON f.id = c.folder_id "
            "  LEFT JOIN capture_match m ON m.id = ("
            "     SELECT id FROM capture_match WHERE capture_id = c.id "
            "     ORDER BY (decided_by = 'user') DESC, confidence DESC LIMIT 1) "
            "  LEFT JOIN taxon t ON t.id = m.taxon_id "
            "  LEFT JOIN geocode_cache g ON g.lat_round = ROUND(c.latitude, 3) "
            "     AND g.lon_round = ROUND(c.longitude, 3) "
            ")") + where + QStringLiteral(
            " ORDER BY (captured_on IS NULL), captured_on DESC, id DESC"));
        if (m_projectScope > 0)
            q.addBindValue(m_projectScope);
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
            row.ext = q.value(10).toString();
            row.hasGps = !q.value(11).isNull() && !q.value(12).isNull();
            if (row.hasGps) {
                row.latitude = q.value(11).toDouble();
                row.longitude = q.value(12).toDouble();
            }
            row.isBestShot = q.value(13).toBool();
            row.locality = q.value(14).toString();

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
