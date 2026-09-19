#pragma once

#include "db/CatalogueDescriptor.h"

#include <QAbstractListModel>
#include <QHash>
#include <QIcon>
#include <QList>
#include <QMetaType>
#include <QString>

class QThread;
class QSqlDatabase;

namespace pl {
class Database;
}
namespace pl::net {
class PhotoCache;
}

namespace pl::model {

// Flat, name-sorted list of the leaf-rank taxa (species and below) in a
// reference tree — or in the subtree of one selected taxon — each shown with
// its iNaturalist reference photo. Thumbnails are pulled lazily from the
// PhotoCache and filled in as downloads arrive.
class ReferencePhotoModel : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Roles {
        InatIdRole = Qt::UserRole + 1,
        NameRole,
        CommonNameRole,
        RankRole,
        AttributionRole,
        PhotoUrlRole,
        HasPhotoRole,
    };

    ReferencePhotoModel(pl::Database &db, pl::net::PhotoCache &photos, QObject *parent = nullptr);
    ~ReferencePhotoModel() override;

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // The reference tree to list. Re-queries.
    void setProject(int projectId);

    // 0 = every leaf taxon in the project; otherwise only those at or below this
    // taxon (by iNat id). Re-queries asynchronously (see below) -- this is the
    // path driven by tree clicks, so it must not block the GUI thread on a
    // remote Postgres round trip.
    void setScope(qint64 taxonInatId);

    void reload();

    int speciesCount() const { return int(m_rows.size()); }
    int withPhotoCount() const;

    // One leaf taxon as loaded by reload()/fetchRows(). Public rather than
    // private so it can cross the async worker thread's queued-connection
    // boundary (see Q_DECLARE_METATYPE below).
    struct Row
    {
        qint64 inatId = 0;
        QString name;
        QString commonName;
        QString rank;
        QString photoUrl;
        QString attribution;
    };

    // Parameters that fully determine which rows a query returns -- shared by
    // the synchronous (reload()) and asynchronous (background-thread) paths.
    struct QueryParams
    {
        int projectId = -1;
        qint64 scope = 0;
    };

signals:
    // Internal: drives the background worker thread. Connected to
    // Worker::run() with an (implicitly queued, cross-thread) connection.
    void requestFetch(quint64 generation, pl::model::ReferencePhotoModel::QueryParams params);

private:
    class Worker;

    static QList<Row> fetchRows(const QString &connectionName, const QueryParams &params);

    void onPhotoReady(const QString &url);
    void ensureWorker();
    void reloadAsync();
    void onRowsReady(quint64 generation, QList<Row> rows);

    pl::Database &m_db;
    pl::net::PhotoCache &m_photos;
    QList<Row> m_rows;
    QHash<QString, QList<int>> m_rowsByUrl;   // photo url -> row indices
    QIcon m_placeholder;
    int m_projectId = -1;
    qint64 m_scope = 0;
    QThread *m_workerThread = nullptr;
    Worker *m_worker = nullptr;
    quint64 m_generation = 0;   // bumped per request; discards superseded replies
};

} // namespace pl::model

Q_DECLARE_METATYPE(pl::model::ReferencePhotoModel::Row)
Q_DECLARE_METATYPE(pl::model::ReferencePhotoModel::QueryParams)
