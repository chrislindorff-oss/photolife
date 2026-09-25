#include "inat/InatDownloadModel.h"

#include "net/PhotoCache.h"
#include "taxonomy/TaxonomyStore.h"

#include <QPainter>
#include <QPixmap>
#include <QStringList>

namespace pl::inat {
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

// A faded copy, so a likely-duplicate candidate reads as visually
// de-emphasised in the grid without being hidden or made unselectable.
QIcon dimmed(const QPixmap &pm)
{
    QPixmap out(pm.size());
    out.fill(Qt::transparent);
    QPainter p(&out);
    p.setOpacity(0.4);
    p.drawPixmap(0, 0, pm);
    p.end();
    return QIcon(out);
}

} // namespace

InatDownloadModel::InatDownloadModel(pl::net::PhotoCache &photos, pl::taxonomy::TaxonomyStore &store,
                                     QObject *parent)
    : QAbstractListModel(parent), m_photos(photos), m_store(store), m_placeholder(makePlaceholder())
{
    connect(&m_photos, &pl::net::PhotoCache::ready, this, &InatDownloadModel::onPhotoReady);
}

int InatDownloadModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_rows.size());
}

QHash<int, QByteArray> InatDownloadModel::roleNames() const
{
    return {
        {ObservationIdRole, "observationId"},
        {PhotoIdRole, "photoId"},
        {TaxonInatIdRole, "taxonInatId"},
        {ObservedOnRole, "observedOn"},
        {DownloadUrlRole, "downloadUrl"},
        {LikelyDuplicateRole, "likelyDuplicate"},
        {HasGpsRole, "hasGps"},
        {LatitudeRole, "latitude"},
        {LongitudeRole, "longitude"},
        {NameRole, "name"},
        {PlaceGuessRole, "placeGuess"},
    };
}

QVariant InatDownloadModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size())
        return {};

    const Row &row = m_rows.at(index.row());

    switch (role) {
    case Qt::DisplayRole: {
        QStringList lines;
        if (!row.taxonName.isEmpty())
            lines << row.taxonName;
        if (!row.taxonCommonName.isEmpty() && row.taxonCommonName != row.taxonName)
            lines << row.taxonCommonName;
        lines << (row.observedOn.isEmpty() ? tr("(no date)") : row.observedOn);
        return lines.join(QLatin1Char('\n'));
    }
    case Qt::ToolTipRole: {
        QString tip = row.taxonName.isEmpty() ? tr("(unidentified)") : row.taxonName;
        tip += QLatin1Char('\n') + (row.observedOn.isEmpty() ? tr("(no date)") : row.observedOn);
        if (row.alreadyDownloaded)
            tip += QStringLiteral("\nAlready downloaded to your library");
        else if (row.likelyDuplicate)
            tip += QStringLiteral("\nPossibly already in your library");
        if (row.presenceChecked) {
            if (!row.inLibrary)
                tip += tr("\nNEW: no photos of this taxon in your library yet");
            if (!row.inAnyTree)
                tip += tr("\nNO TREE: this taxon isn't in any of your reference trees");
        }
        return tip;
    }
    case Qt::DecorationRole: {
        const bool faded = row.likelyDuplicate || row.alreadyDownloaded;
        QPixmap pm = row.previewUrl.isEmpty() ? QPixmap() : m_photos.photo(row.previewUrl);
        if (pm.isNull()) {
            if (!faded)
                return m_placeholder;
            pm = m_placeholder.pixmap(net::PhotoCache::kMaxEdge, net::PhotoCache::kMaxEdge);
        }
        return faded ? dimmed(pm) : QIcon(pm);
    }
    case ObservationIdRole:
        return row.observationId;
    case PhotoIdRole:
        return row.photoId;
    case TaxonInatIdRole:
        return row.taxonInatId;
    case ObservedOnRole:
        return row.observedOn;
    case DownloadUrlRole:
        return row.downloadUrl;
    case LikelyDuplicateRole:
        // Covers both the taxon+date+GPS heuristic and an exact
        // already-downloaded photo-id match -- both render faded and are
        // excluded the same way from Select All / "Hide faded photos"
        // (see MainWindow), so callers don't need to know which applies.
        return row.likelyDuplicate || row.alreadyDownloaded;
    case HasGpsRole:
        return row.latitude.has_value() && row.longitude.has_value();
    case LatitudeRole:
        return row.latitude ? QVariant(*row.latitude) : QVariant();
    case LongitudeRole:
        return row.longitude ? QVariant(*row.longitude) : QVariant();
    case NameRole:
        return row.taxonName;
    case PlaceGuessRole:
        return row.placeGuess;
    case PresenceCheckedRole:
        return row.presenceChecked;
    case InLibraryRole:
        return row.inLibrary;
    case InAnyTreeRole:
        return row.inAnyTree;
    default:
        return {};
    }
}

void InatDownloadModel::setCandidates(const QList<Candidate> &candidates)
{
    beginResetModel();
    m_rows.clear();
    m_rowsByUrl.clear();

    // Several photos usually share the same observation's taxon; avoid
    // re-querying the store for each one.
    QHash<qint64, taxonomy::TaxonomyStore::TaxonPhoto> nameCache;
    auto taxonFor = [&](qint64 taxonInatId) -> const taxonomy::TaxonomyStore::TaxonPhoto & {
        auto it = nameCache.find(taxonInatId);
        if (it == nameCache.end())
            it = nameCache.insert(taxonInatId, m_store.taxonPhoto(taxonInatId));
        return it.value();
    };

    for (const Candidate &c : candidates) {
        const auto &taxon = taxonFor(c.observation.taxonInatId);
        for (const taxonomy::ObservationPhoto &photo : c.observation.photos) {
            Row row;
            row.observationId = c.observation.id;
            row.photoId = photo.id;
            row.taxonInatId = c.observation.taxonInatId;
            row.observedOn = c.observation.observedOn;
            row.previewUrl = photo.previewUrl;
            row.downloadUrl = photo.downloadUrl;
            row.likelyDuplicate = c.likelyDuplicate;
            row.alreadyDownloaded = c.alreadyDownloadedPhotoIds.contains(photo.id);
            row.latitude = c.observation.latitude;
            row.longitude = c.observation.longitude;
            // The local cache first (the tree's own spelling); iNat's own
            // names for taxa outside every cached tree.
            row.taxonName = !taxon.name.isEmpty() ? taxon.name : c.observation.taxonName;
            row.taxonCommonName =
                !taxon.commonName.isEmpty() ? taxon.commonName : c.observation.taxonCommonName;
            row.placeGuess = c.observation.placeGuess;
            row.presenceChecked = c.presenceChecked;
            row.inLibrary = c.inLibrary;
            row.inAnyTree = c.inAnyTree;
            if (!row.previewUrl.isEmpty())
                m_rowsByUrl[row.previewUrl].append(int(m_rows.size()));
            m_rows.append(std::move(row));
        }
    }

    endResetModel();
}

void InatDownloadModel::clear()
{
    setCandidates({});
}

void InatDownloadModel::onPhotoReady(const QString &url)
{
    const auto it = m_rowsByUrl.constFind(url);
    if (it == m_rowsByUrl.constEnd())
        return;
    for (int r : *it) {
        const QModelIndex idx = index(r);
        emit dataChanged(idx, idx, {Qt::DecorationRole});
    }
}

} // namespace pl::inat
