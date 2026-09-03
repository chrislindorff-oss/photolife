#pragma once

#include <QHash>
#include <QObject>
#include <QPixmap>
#include <QSet>
#include <QString>

#include <functional>

namespace pl::thumb {

// Two-level thumbnail store: a small in-memory pixmap cache over an on-disk JPEG
// cache, both keyed by a rendition's content hash so a moved or renamed file
// keeps its thumbnail. Generation happens off the calling thread; ask with
// thumbnail(), and connect ready() to refresh when a miss has been filled.
class ThumbnailCache : public QObject
{
    Q_OBJECT

public:
    // Longest-edge pixel sizes the cache produces.
    static constexpr int kGridPx = 256;
    static constexpr int kPreviewPx = 1024;

    explicit ThumbnailCache(QString cacheDir, QObject *parent = nullptr);
    ~ThumbnailCache() override;

    // A function that renders a source file (used for formats QImageReader can't
    // read, i.e. camera RAW) to a QImage no smaller than `longestEdge`. Returns a
    // null QImage on failure. Set by the RAW module when it is available.
    using SourceLoader = std::function<QImage(const QString &path, int longestEdge)>;
    void setRawLoader(SourceLoader loader);

    // Returns the thumbnail if it is already in memory or on disk; otherwise
    // returns a null pixmap and schedules generation, emitting ready() later.
    QPixmap thumbnail(const QString &contentHash, const QString &sourcePath, int longestEdge);

    // True once a disk file exists for this key (no generation scheduled).
    bool isCached(const QString &contentHash, int longestEdge) const;

    QString cacheDir() const { return m_cacheDir; }

signals:
    void ready(const QString &contentHash, int longestEdge);

private:
    QString diskPath(const QString &contentHash, int longestEdge) const;
    void schedule(const QString &contentHash, const QString &sourcePath, int longestEdge);
    void onGenerated(const QString &memKey, const QString &contentHash, int longestEdge,
                     const QImage &image);

    QString m_cacheDir;
    mutable QHash<QString, QPixmap> m_memory;   // "px:hash" -> pixmap
    QSet<QString> m_pending;                    // "px:hash" in flight
    SourceLoader m_rawLoader;
};

} // namespace pl::thumb
