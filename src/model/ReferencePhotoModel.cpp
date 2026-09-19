#include "model/ReferencePhotoModel.h"

#include "db/Database.h"
#include "net/PhotoCache.h"
#include "taxonomy/TaxonomyStore.h"

#include <QPainter>
#include <QPixmap>
#include <QThread>

namespace pl::model {
namespace {

bool registerMetaTypes()
{
    qRegisterMetaType<pl::model::ReferencePhotoModel::QueryParams>(
        "pl::model::ReferencePhotoModel::QueryParams");
    qRegisterMetaType<QList<pl::model::ReferencePhotoModel::Row>>(
        "QList<pl::model::ReferencePhotoModel::Row>");
    return true;
}
const bool kMetaTypesRegistered = registerMetaTypes();

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

// See CaptureListModel::Worker -- same persistent background-connection
// pattern, applied here so a tree click's setScope() doesn't block the GUI
// thread on a remote Postgres round trip either.
class ReferencePhotoModel::Worker : public QObject
{
    Q_OBJECT

public:
    explicit Worker(CatalogueDescriptor descriptor) : m_descriptor(std::move(descriptor)) {}

public slots:
    void run(quint64 generation, pl::model::ReferencePhotoModel::QueryParams params)
    {
        if (!m_db.isOpen() && !m_db.open(m_descriptor))
            return;
        const QList<ReferencePhotoModel::Row> rows =
            ReferencePhotoModel::fetchRows(m_db.connectionName(), params);
        emit rowsReady(generation, rows);
    }

    // See CaptureListModel::Worker::shutdown() -- closes the connection on
    // this worker's own thread before the model tears the thread down.
    void shutdown() { m_db.close(); }

signals:
    void rowsReady(quint64 generation, QList<pl::model::ReferencePhotoModel::Row> rows);

private:
    CatalogueDescriptor m_descriptor;
    Database m_db;
};

ReferencePhotoModel::ReferencePhotoModel(pl::Database &db, pl::net::PhotoCache &photos,
                                        QObject *parent)
    : QAbstractListModel(parent), m_db(db), m_photos(photos), m_placeholder(makePlaceholder())
{
    Q_UNUSED(kMetaTypesRegistered);
    connect(&m_photos, &pl::net::PhotoCache::ready, this, &ReferencePhotoModel::onPhotoReady);
}

ReferencePhotoModel::~ReferencePhotoModel()
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
    reloadAsync();
}

void ReferencePhotoModel::reload()
{
    beginResetModel();
    m_rows = m_db.isOpen() ? fetchRows(m_db.connectionName(), QueryParams{m_projectId, m_scope})
                           : QList<Row>();
    m_rowsByUrl.clear();
    for (int i = 0; i < m_rows.size(); ++i) {
        if (!m_rows[i].photoUrl.isEmpty())
            m_rowsByUrl[m_rows[i].photoUrl].append(i);
    }
    endResetModel();
}

void ReferencePhotoModel::ensureWorker()
{
    if (m_worker)
        return;
    m_workerThread = new QThread();
    m_worker = new Worker(m_db.descriptor());
    m_worker->moveToThread(m_workerThread);
    connect(this, &ReferencePhotoModel::requestFetch, m_worker, &Worker::run);
    connect(m_worker, &Worker::rowsReady, this, &ReferencePhotoModel::onRowsReady);
    m_workerThread->start();
}

void ReferencePhotoModel::reloadAsync()
{
    ensureWorker();
    const quint64 generation = ++m_generation;
    emit requestFetch(generation, QueryParams{m_projectId, m_scope});
}

void ReferencePhotoModel::onRowsReady(quint64 generation, QList<Row> rows)
{
    if (generation != m_generation)
        return;   // superseded by a later request -- discard

    beginResetModel();
    m_rows = std::move(rows);
    m_rowsByUrl.clear();
    for (int i = 0; i < m_rows.size(); ++i) {
        if (!m_rows[i].photoUrl.isEmpty())
            m_rowsByUrl[m_rows[i].photoUrl].append(i);
    }
    endResetModel();
}

QList<ReferencePhotoModel::Row> ReferencePhotoModel::fetchRows(const QString &connectionName,
                                                               const QueryParams &params)
{
    QList<Row> rows;
    if (params.projectId <= 0)
        return rows;

    taxonomy::TaxonomyStore store(connectionName);
    for (const auto &lp : store.projectLeafPhotos(params.projectId, params.scope)) {
        Row row;
        row.inatId = lp.inatId;
        row.name = lp.name;
        row.commonName = lp.commonName;
        row.rank = lp.rank;
        row.photoUrl = lp.photoUrl;
        row.attribution = lp.attribution;
        rows.append(std::move(row));
    }
    return rows;
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

#include "ReferencePhotoModel.moc"
