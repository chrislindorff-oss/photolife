#include "model/ReviewQueueModel.h"

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
    return QIcon(pm);
}

} // namespace

ReviewQueueModel::ReviewQueueModel(pl::Database &db, pl::thumb::ThumbnailCache &thumbs,
                                   QObject *parent)
    : QAbstractListModel(parent), m_db(db), m_thumbs(thumbs), m_placeholder(makePlaceholder())
{
    connect(&m_thumbs, &pl::thumb::ThumbnailCache::ready,
            this, &ReviewQueueModel::onThumbnailReady);
}

int ReviewQueueModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_rows.size());
}

QVariant ReviewQueueModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size())
        return {};
    const Row &row = m_rows.at(index.row());

    switch (role) {
    case Qt::DisplayRole:
        return row.nameText.isEmpty() ? row.baseName : row.nameText;
    case Qt::ToolTipRole: {
        QString tip = row.folderPath;
        if (!row.guessName.isEmpty())
            tip += QStringLiteral("\nguess: %1 (%2%)")
                       .arg(row.guessName)
                       .arg(int(row.confidence * 100));
        if (!row.note.isEmpty())
            tip += QStringLiteral("\n%1").arg(row.note);
        return tip;
    }
    case Qt::DecorationRole: {
        if (row.previewPath.isEmpty())
            return m_placeholder;
        const QPixmap pm = m_thumbs.thumbnail(row.previewHash, row.previewPath, kThumbPx);
        return pm.isNull() ? m_placeholder : QIcon(pm);
    }
    case CaptureIdRole:      return row.id;
    case FolderIdRole:       return row.folderId;
    case FolderPathRole:     return row.folderPath;
    case BaseNameRole:       return row.baseName;
    case NameTextRole:       return row.nameText;
    case PreviewPathRole:    return row.previewPath;
    case GuessNameRole:      return row.guessName;
    case GuessInatIdRole:    return row.guessInatId;
    case ConfidenceRole:     return row.confidence;
    case NoteRole:           return row.note;
    case QualifierRole:      return row.qualifier;
    case SearchTextRole:
        return QStringList{row.nameText, row.baseName, row.folderPath, row.guessName}
            .join(QLatin1Char(' '));
    case HasCandidateRole:   return row.guessInatId > 0;
    default:                 return {};
    }
}

void ReviewQueueModel::reload()
{
    beginResetModel();
    m_rows.clear();
    m_rowsByHash.clear();

    if (m_db.isOpen()) {
        QSqlQuery q(QSqlDatabase::database(m_db.connectionName(), false));
        q.setForwardOnly(true);
        q.exec(QStringLiteral(
            "SELECT c.id, c.folder_id, f.path, c.base_name, c.name_text, "
            "  (SELECT r.path FROM rendition r WHERE r.capture_id = c.id "
            "     ORDER BY (r.kind='raw'), r.id LIMIT 1), "
            "  (SELECT r.content_hash FROM rendition r WHERE r.capture_id = c.id "
            "     ORDER BY (r.kind='raw'), r.id LIMIT 1), "
            "  m.confidence, m.note, m.qualifier, t.name, t.inat_id "
            "FROM capture c JOIN folder f ON f.id = c.folder_id "
            "LEFT JOIN capture_match m ON m.id = ("
            "   SELECT id FROM capture_match WHERE capture_id = c.id "
            "   ORDER BY (decided_by='user') DESC, confidence DESC LIMIT 1) "
            "LEFT JOIN taxon t ON t.id = m.taxon_id "
            "WHERE m.status IS NULL OR m.status = 'pending' "
            "ORDER BY f.path, c.base_name"));

        while (q.next()) {
            Row row;
            row.id = q.value(0).toLongLong();
            row.folderId = q.value(1).toInt();
            row.folderPath = q.value(2).toString();
            row.baseName = q.value(3).toString();
            row.nameText = q.value(4).toString();
            row.previewPath = q.value(5).toString();
            row.previewHash = q.value(6).toString();
            row.confidence = q.value(7).toDouble();
            row.note = q.value(8).toString();
            row.qualifier = q.value(9).toString();
            row.guessName = q.value(10).toString();
            row.guessInatId = q.value(11).toLongLong();
            if (!row.previewHash.isEmpty())
                m_rowsByHash[row.previewHash].append(int(m_rows.size()));
            m_rows.append(std::move(row));
        }
    }

    endResetModel();
}

void ReviewQueueModel::dropCapture(qint64 captureId)
{
    for (int i = 0; i < m_rows.size(); ++i) {
        if (m_rows.at(i).id != captureId)
            continue;
        beginRemoveRows({}, i, i);
        m_rows.removeAt(i);
        m_rowsByHash.clear();
        for (int r = 0; r < m_rows.size(); ++r) {
            if (!m_rows.at(r).previewHash.isEmpty())
                m_rowsByHash[m_rows.at(r).previewHash].append(r);
        }
        endRemoveRows();
        return;
    }
}

int ReviewQueueModel::noCandidateCount() const
{
    int n = 0;
    for (const Row &row : m_rows) {
        if (row.guessInatId <= 0)
            ++n;
    }
    return n;
}

int ReviewQueueModel::pendingInFolder(int folderId) const
{
    int n = 0;
    for (const Row &row : m_rows) {
        if (row.folderId == folderId)
            ++n;
    }
    return n;
}

int ReviewQueueModel::pendingUnderFolder(const QString &folderPath) const
{
    int n = 0;
    const QString prefix = folderPath + QLatin1Char('/');
    for (const Row &row : m_rows) {
        if (row.folderPath == folderPath || row.folderPath.startsWith(prefix))
            ++n;
    }
    return n;
}

void ReviewQueueModel::onThumbnailReady(const QString &contentHash, int longestEdge)
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
