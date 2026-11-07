#include "model/LifeListModel.h"

#include "scan/Exif.h"
#include "thumb/ThumbnailCache.h"

#include <QDate>
#include <QDateTime>
#include <QFont>
#include <QPainter>
#include <QPixmap>

#include <limits>

namespace pl::model {

namespace {

constexpr int kThumbPx = thumb::ThumbnailCache::kGridPx;

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

LifeListModel::LifeListModel(thumb::ThumbnailCache &thumbs, QObject *parent)
    : QAbstractTableModel(parent), m_thumbs(thumbs), m_placeholder(makePlaceholder())
{
    connect(&m_thumbs, &thumb::ThumbnailCache::ready, this, &LifeListModel::onThumbnailReady);
}

void LifeListModel::setEntries(const QList<coverage::LifeListEntry> &entries)
{
    beginResetModel();
    m_entries = entries;
    m_rowsByKey.clear();
    m_rowsByCapture.clear();
    for (int r = 0; r < m_entries.size(); ++r) {
        const auto &e = m_entries[r];
        m_rowsByCapture[e.captureId] << r;
        if (!e.previewPath.isEmpty())
            m_rowsByKey[thumb::ThumbnailCache::keyFor(e.previewHash, e.previewPath)] << r;
    }
    endResetModel();
}

void LifeListModel::setShowThumbnails(bool on)
{
    if (m_thumbnails == on)
        return;
    m_thumbnails = on;
    if (!m_entries.isEmpty())
        emit dataChanged(index(0, PhotoColumn), index(int(m_entries.size()) - 1, PhotoColumn), {Qt::DecorationRole});
}

void LifeListModel::setCameraInfo(int captureId, const QString &camera, std::optional<double> focalLength)
{
    for (int r : m_rowsByCapture.value(captureId)) {
        coverage::LifeListEntry &e = m_entries[r];
        e.camera = camera;
        e.focalLength = focalLength;
        e.cameraChecked = true;
        emit dataChanged(index(r, CameraColumn), index(r, FocalColumn));
    }
}

QString LifeListModel::dateText(const QString &capturedOn)
{
    const QDate d = QDate::fromString(capturedOn.left(10), Qt::ISODate);
    return d.isValid() ? d.toString(QStringLiteral("d MMM yyyy")) : tr("No date");
}

int LifeListModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_entries.size());
}

int LifeListModel::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant LifeListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_entries.size())
        return {};
    const coverage::LifeListEntry &e = m_entries[index.row()];
    const int col = index.column();
    switch (role) {
    case Qt::DisplayRole:
        switch (col) {
        case NumberColumn:
            return e.number > 0 ? QString::number(e.number) : QStringLiteral("—");
        case DateColumn:
            return dateText(e.firstOn);
        case CommonColumn:
            return e.commonName == e.name ? QString() : e.commonName;
        case ScientificColumn:
            return e.name;
        case PlaceColumn:
            return e.locality;
        case CameraColumn:
            return e.camera;
        case FocalColumn:
            return e.focalLength ? scan::focalText(*e.focalLength) : QString();
        case PhotosColumn:
            return e.photoCount;
        default:
            return {};
        }
    case Qt::DecorationRole:
        if (col != PhotoColumn || !m_thumbnails)
            return {};
        if (e.previewPath.isEmpty())
            return m_placeholder;
        {
            const QPixmap pm = m_thumbs.thumbnail(e.previewHash, e.previewPath, kThumbPx);
            return pm.isNull() ? m_placeholder : QIcon(pm);
        }
    case Qt::FontRole:
        if (col == ScientificColumn) {
            QFont f;
            f.setItalic(true);
            return f;
        }
        return {};
    case Qt::TextAlignmentRole:
        if (col == NumberColumn || col == PhotosColumn || col == FocalColumn)
            return int(Qt::AlignRight | Qt::AlignVCenter);
        return int(Qt::AlignLeft | Qt::AlignVCenter);
    case Qt::ToolTipRole:
        if (col == PhotoColumn || col == NumberColumn)
            return tr("Double-click, or right-click, to see this photo full screen");
        if (col == DateColumn && e.firstOn.isEmpty())
            return tr("None of its photos has a usable date");
        if (col == ScientificColumn && e.belowSpecies)
            return tr("%1 (a %2)").arg(e.name, e.rank.toLower());
        if ((col == CameraColumn || col == FocalColumn) && !e.cameraChecked && !e.previewPath.isEmpty())
            return tr("Reading the first photo's details…");
        if ((col == CameraColumn || col == FocalColumn) && e.cameraChecked)
            return tr("From the first photo's EXIF");
        return {};
    case SortRole:
        switch (col) {
        case NumberColumn:
        case DateColumn:
            // Undated last, whichever way it's sorted from the start.
            return e.number > 0 ? e.number : std::numeric_limits<int>::max();
        case PhotoColumn:
            return index.row();
        case CommonColumn:
            return (e.commonName.isEmpty() || e.commonName == e.name ? e.name : e.commonName).toLower();
        case ScientificColumn:
            return e.name.toLower();
        case PlaceColumn:
            return e.locality.toLower();
        case CameraColumn:
            // Unknown after every name (A to Z).
            return e.camera.isEmpty() ? QStringLiteral("\uffff") : e.camera.toLower();
        case FocalColumn:
            return e.focalLength ? *e.focalLength : std::numeric_limits<double>::max();
        case PhotosColumn:
            return e.photoCount;
        default:
            return {};
        }
    case CaptureIdRole:
        return e.captureId;
    case InatIdRole:
        return e.inatId;
    case PreviewPathRole:
        return e.previewPath;
    default:
        return {};
    }
}

QVariant LifeListModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (section) {
    case NumberColumn:
        return tr("#");
    case PhotoColumn:
        return tr("Photo");
    case DateColumn:
        return tr("First photographed");
    case CommonColumn:
        return tr("Common name");
    case ScientificColumn:
        return tr("Scientific name");
    case PlaceColumn:
        return tr("Where");
    case CameraColumn:
        return tr("Camera");
    case FocalColumn:
        return tr("Focal length");
    case PhotosColumn:
        return tr("Photos");
    default:
        return {};
    }
}

void LifeListModel::onThumbnailReady(const QString &key, int longestEdge)
{
    if (longestEdge != kThumbPx || !m_thumbnails)
        return;
    const auto it = m_rowsByKey.constFind(key);
    if (it == m_rowsByKey.constEnd())
        return;
    for (int r : *it) {
        const QModelIndex idx = index(r, PhotoColumn);
        emit dataChanged(idx, idx, {Qt::DecorationRole});
    }
}

} // namespace pl::model
