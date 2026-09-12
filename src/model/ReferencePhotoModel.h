#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QIcon>
#include <QList>
#include <QString>

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

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // The reference tree to list. Re-queries.
    void setProject(int projectId);

    // 0 = every leaf taxon in the project; otherwise only those at or below this
    // taxon (by iNat id). Re-queries.
    void setScope(qint64 taxonInatId);

    void reload();

    int speciesCount() const { return int(m_rows.size()); }
    int withPhotoCount() const;

private:
    struct Row
    {
        qint64 inatId = 0;
        QString name;
        QString commonName;
        QString rank;
        QString photoUrl;
        QString attribution;
    };

    void onPhotoReady(const QString &url);

    pl::Database &m_db;
    pl::net::PhotoCache &m_photos;
    QList<Row> m_rows;
    QHash<QString, QList<int>> m_rowsByUrl;   // photo url -> row indices
    QIcon m_placeholder;
    int m_projectId = -1;
    qint64 m_scope = 0;
};

} // namespace pl::model
