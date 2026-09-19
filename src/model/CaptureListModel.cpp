#include "model/CaptureListModel.h"

#include "db/Database.h"
#include "thumb/ThumbnailCache.h"

#include <QPainter>
#include <QPixmap>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStringList>
#include <QThread>

namespace pl::model {
namespace {

constexpr int kThumbPx = pl::thumb::ThumbnailCache::kGridPx;

bool registerMetaTypes()
{
    qRegisterMetaType<pl::model::CaptureListModel::QueryParams>(
        "pl::model::CaptureListModel::QueryParams");
    qRegisterMetaType<QList<pl::model::CaptureListModel::Row>>(
        "QList<pl::model::CaptureListModel::Row>");
    return true;
}
const bool kMetaTypesRegistered = registerMetaTypes();

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

// Runs fetchRows() on its own thread with its own catalogue connection, so a
// tree click's query never blocks the GUI thread -- important once the
// catalogue is a shared Postgres connection where each round trip can be tens
// of milliseconds. Persistent (not one-shot like ScanService/MatchService's
// workers): tree clicks are frequent, so a fresh connection per click would
// undercut the point of the fix.
class CaptureListModel::Worker : public QObject
{
    Q_OBJECT

public:
    explicit Worker(CatalogueDescriptor descriptor) : m_descriptor(std::move(descriptor)) {}

public slots:
    void run(quint64 generation, pl::model::CaptureListModel::QueryParams params)
    {
        if (!m_db.isOpen() && !m_db.open(m_descriptor))
            return;
        const QList<CaptureListModel::Row> rows = CaptureListModel::fetchRows(
            QSqlDatabase::database(m_db.connectionName(), false), params);
        emit rowsReady(generation, rows);
    }

    // Closes the connection while still running on this worker's own thread
    // -- Qt's SQL drivers tie a connection to the thread that opened it, and
    // closing/destroying it from elsewhere (e.g. the model's destructor,
    // running on the GUI thread, deleting this Worker after the thread has
    // already stopped) trips "database does not belong to the calling
    // thread". Invoked with a blocking queued connection just before the
    // model tears the worker thread down.
    void shutdown() { m_db.close(); }

signals:
    void rowsReady(quint64 generation, QList<pl::model::CaptureListModel::Row> rows);

private:
    CatalogueDescriptor m_descriptor;
    Database m_db;
};

CaptureListModel::CaptureListModel(pl::Database &db, pl::thumb::ThumbnailCache &thumbs,
                                   QObject *parent)
    : QAbstractListModel(parent), m_db(db), m_thumbs(thumbs), m_placeholder(makePlaceholder())
{
    Q_UNUSED(kMetaTypesRegistered);
    connect(&m_thumbs, &pl::thumb::ThumbnailCache::ready,
            this, &CaptureListModel::onThumbnailReady);
}

CaptureListModel::~CaptureListModel()
{
    if (m_workerThread) {
        if (m_worker)
            QMetaObject::invokeMethod(m_worker, "shutdown", Qt::BlockingQueuedConnection);
        m_workerThread->quit();
        m_workerThread->wait();
    }
    delete m_worker;
    delete m_workerThread;
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
        {MatchedTaxonInatIdRole, "matchedTaxonInatId"},
        {PreviewIsRawRole, "previewIsRaw"},
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
    case MatchedTaxonInatIdRole:
        return row.matchedTaxonInatId;
    case PreviewIsRawRole:
        return row.previewIsRaw;
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
    reloadAsync();
}

void CaptureListModel::setProjectScope(int projectId)
{
    const int normalised = projectId > 0 ? projectId : 0;
    if (m_projectScope == normalised)
        return;
    m_projectScope = normalised;
    reloadAsync();
}

void CaptureListModel::setScope(int projectId, qint64 taxonInatId)
{
    const int normalised = projectId > 0 ? projectId : 0;
    if (m_projectScope == normalised && m_taxonScope == taxonInatId)
        return;
    m_projectScope = normalised;
    m_taxonScope = taxonInatId;
    reloadAsync();
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

void CaptureListModel::applyGps(int captureId, double latitude, double longitude)
{
    for (int i = 0; i < m_rows.size(); ++i) {
        if (m_rows[i].id != captureId)
            continue;
        m_rows[i].hasGps = true;
        m_rows[i].latitude = latitude;
        m_rows[i].longitude = longitude;
        const QModelIndex idx = index(i);
        emit dataChanged(idx, idx, {HasGpsRole, LatitudeRole, LongitudeRole});
        return;
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
        const QueryParams params{m_statusFilter, m_taxonScope, m_projectScope, m_bestShotOnly};
        m_rows = fetchRows(QSqlDatabase::database(m_db.connectionName(), false), params);
        for (int i = 0; i < m_rows.size(); ++i) {
            if (!m_rows[i].previewHash.isEmpty())
                m_rowsByHash[m_rows[i].previewHash].append(i);
        }
    }

    endResetModel();
}

void CaptureListModel::ensureWorker()
{
    if (m_worker)
        return;
    m_workerThread = new QThread();
    m_worker = new Worker(m_db.descriptor());
    m_worker->moveToThread(m_workerThread);
    connect(this, &CaptureListModel::requestFetch, m_worker, &Worker::run);
    connect(m_worker, &Worker::rowsReady, this, &CaptureListModel::onRowsReady);
    m_workerThread->start();
}

void CaptureListModel::reloadAsync()
{
    ensureWorker();
    const quint64 generation = ++m_generation;
    emit requestFetch(generation, QueryParams{m_statusFilter, m_taxonScope, m_projectScope,
                                              m_bestShotOnly});
}

void CaptureListModel::onRowsReady(quint64 generation, QList<Row> rows)
{
    if (generation != m_generation)
        return;   // superseded by a later request -- discard

    beginResetModel();
    m_rows = std::move(rows);
    m_rowsByHash.clear();
    for (int i = 0; i < m_rows.size(); ++i) {
        if (!m_rows[i].previewHash.isEmpty())
            m_rowsByHash[m_rows[i].previewHash].append(i);
    }
    endResetModel();
}

QList<CaptureListModel::Row> CaptureListModel::fetchRows(QSqlDatabase db,
                                                         const QueryParams &params)
{
    QList<Row> rows;
    if (!db.isOpen())
        return rows;

    {
        QStringList clauses;
        if (params.statusFilter == QLatin1String("auto"))
            clauses << QStringLiteral("match_status = 'auto'");
        else if (params.statusFilter == QLatin1String("confirmed"))
            clauses << QStringLiteral("match_status = 'confirmed'");
        else if (params.statusFilter == QLatin1String("pending"))
            clauses << QStringLiteral("match_status = 'pending' AND matched_id IS NOT NULL");
        else if (params.statusFilter == QLatin1String("unmatched"))
            clauses << QStringLiteral(
                "(match_status IS NULL OR (match_status = 'pending' AND matched_id IS NULL))");

        const qint64 taxonScope = params.taxonScope;
        const int projectScope = params.projectScope;
        if (projectScope > 0 && taxonScope > 0) {
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
        } else if (projectScope > 0) {
            // No taxon picked: still confine to this reference tree rather than
            // falling back to the whole library.
            clauses << QStringLiteral(
                "matched_id IN (SELECT taxon_id FROM project_taxon WHERE project_id = ?)");
        } else if (taxonScope > 0) {
            clauses << QStringLiteral(
                "matched_id IN (SELECT id FROM taxon WHERE inat_id IN ("
                "  WITH RECURSIVE sub(x) AS (SELECT ? "
                "    UNION ALL SELECT t.inat_id FROM taxon t JOIN sub ON t.parent_inat_id = sub.x) "
                "  SELECT x FROM sub))");
        }

        if (params.bestShotOnly)
            // Not "is_best_shot = 1": is_best_shot comes from EXISTS(...), an
            // actual boolean on Postgres (comparing it to an integer literal
            // is a type error there), but a plain truthy 0/1 on SQLite. A
            // bare boolean-context reference is valid truthiness on both.
            clauses << QStringLiteral("is_best_shot");

        const QString where =
            clauses.isEmpty() ? QString()
                              : QStringLiteral(" WHERE ") + clauses.join(QStringLiteral(" AND "));

        QSqlQuery q(db);
        q.setForwardOnly(true);
        q.prepare(QStringLiteral(
            "SELECT id, base_name, name_text, captured_on, date_source, path, preview_path, "
            "       preview_hash, match_status, matched_name, preview_ext, latitude, longitude, "
            "       is_best_shot, locality, matched_taxon_inat_id, preview_kind "
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
            "    g.locality AS locality, t.inat_id AS matched_taxon_inat_id, "
            "    (SELECT r.kind FROM rendition r WHERE r.capture_id = c.id "
            "       ORDER BY (r.kind = 'raw'), r.id LIMIT 1) AS preview_kind "
            "  FROM capture c JOIN folder f ON f.id = c.folder_id "
            "  LEFT JOIN capture_match m ON m.id = ("
            "     SELECT id FROM capture_match WHERE capture_id = c.id "
            "     ORDER BY (decided_by = 'user') DESC, confidence DESC LIMIT 1) "
            "  LEFT JOIN taxon t ON t.id = m.taxon_id "
            // CAST(... AS NUMERIC): Postgres's two-argument ROUND() has no
            // overload for a bare double precision/real column; the cast is
            // a no-op on SQLite.
            "  LEFT JOIN geocode_cache g ON g.lat_round = ROUND(CAST(c.latitude AS NUMERIC), 3) "
            "     AND g.lon_round = ROUND(CAST(c.longitude AS NUMERIC), 3) "
            ")") + where + QStringLiteral(
            " ORDER BY (captured_on IS NULL), captured_on DESC, id DESC"));
        if (projectScope > 0)
            q.addBindValue(projectScope);
        if (taxonScope > 0)
            q.addBindValue(qlonglong(taxonScope));
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
            row.matchedTaxonInatId = q.value(15).toLongLong();
            row.previewIsRaw = q.value(16).toString() == QLatin1String("raw");

            rows.append(std::move(row));
        }
    }

    return rows;
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

#include "CaptureListModel.moc"
