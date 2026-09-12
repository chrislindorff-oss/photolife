#pragma once

#include <QHash>
#include <QObject>
#include <QPixmap>
#include <QQueue>
#include <QSet>
#include <QString>
#include <QUrl>

class QNetworkAccessManager;
class QNetworkReply;

namespace pl::net {

// A two-level cache for OpenStreetMap raster tiles: an in-memory QPixmap cache
// over an on-disk PNG cache, both keyed by the tile's (z, x, y) coordinate.
// Downloads run through a private QNetworkAccessManager, capped low per OSM's
// tile usage policy (identify the client, keep concurrency modest, cache
// aggressively, never re-request what's already cached).
//
// Ask with tile(); connect ready() to repaint when a miss has been filled.
// Lives on and is used from the GUI thread.
class TileCache : public QObject
{
    Q_OBJECT

public:
    static constexpr int kTileSize = 256;

    TileCache(QString cacheDir, QByteArray userAgent, QObject *parent = nullptr);
    ~TileCache() override;

    // The pixmap for tile (z, x, y) if already in memory or on disk; otherwise
    // a null pixmap and a scheduled download that emits ready(z, x, y) on
    // completion. A tile that has already failed to download stays null and
    // is not retried.
    QPixmap tile(int z, int x, int y);

    // Where the on-disk cache lives, for reporting total storage use.
    QString cacheDir() const { return m_cacheDir; }

    // Bracket a paint pass with beginFrame()/endFrame() around the tile()
    // calls for the tiles currently on screen. Panning or zooming quickly
    // requests a different tile set on every repaint; without this, tiles
    // left over from earlier, now off-screen viewports keep queuing ahead of
    // the ones actually needed, and since downloads are capped low (see
    // kMaxConcurrent) the current view can be stuck waiting behind a long
    // backlog of tiles nobody wants anymore. endFrame() drops any
    // not-yet-started download that wasn't requested since the matching
    // beginFrame().
    void beginFrame();
    void endFrame();

signals:
    void ready(int z, int x, int y);

private:
    struct Key
    {
        int z = 0, x = 0, y = 0;
        bool operator==(const Key &o) const { return z == o.z && x == o.x && y == o.y; }
    };
    friend size_t qHash(const Key &k, size_t seed);

    void pump();
    void startDownload(Key key);
    void onFinished(Key key, QNetworkReply *reply);
    QString diskPath(Key key) const;
    QUrl tileUrl(Key key) const;

    QString m_cacheDir;
    QByteArray m_userAgent;
    QNetworkAccessManager *m_nam;

    QHash<Key, QPixmap> m_memory;
    QSet<Key> m_pending;
    QSet<Key> m_failed;
    QQueue<Key> m_waiting;
    QSet<Key> m_neededThisFrame;
    int m_inFlight = 0;

    static constexpr int kMaxConcurrent = 2;   // be a good citizen of the shared OSM tile server
};

} // namespace pl::net
