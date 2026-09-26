#pragma once

#include <QByteArray>
#include <QString>
#include <QUrl>

#include <optional>

namespace pl::net {

// The conditional-GET cache behind HttpClient: one row per URL holding the
// last ETag / Last-Modified and the body to serve on a 304. It lives in its
// own SQLite file under the app's cache directory rather than in the
// catalogue, so it never bloats (or is backed up with) the catalogue and can
// be deleted at any time -- every entry is re-fetchable, and nothing reads it
// except to answer a 304.
//
// Growth is bounded two ways: responses that are unlikely ever to be
// requested again at the same URL are not stored (see isCacheable), and
// open() prunes entries not used within maxAgeDays, then the least recently
// used ones until the stored bodies fit in maxBytes.
//
// Owns a private QSqlDatabase connection, so use it from the thread that
// opened it.
class HttpCache
{
public:
    struct Entry
    {
        QByteArray body;
        QString etag;
        QString lastModified;
        int status = 0;
    };

    static constexpr int kDefaultMaxAgeDays = 30;
    static constexpr qint64 kDefaultMaxBytes = 200LL * 1024 * 1024;

    HttpCache();
    ~HttpCache();

    HttpCache(const HttpCache &) = delete;
    HttpCache &operator=(const HttpCache &) = delete;

    // `path` may be ":memory:" (tests). Prunes on open; false if the file
    // can't be opened, in which case the cache just stays disabled.
    bool open(const QString &path, int maxAgeDays = kDefaultMaxAgeDays,
              qint64 maxBytes = kDefaultMaxBytes);
    bool isOpen() const;

    // Paged or cursor-driven listings (`page=`, `id_above=`), the user's own
    // observations, and batched `/taxa/1,2,3` lookups are keyed by URLs that
    // change from one run to the next, so storing them only accumulates
    // bodies that are never revalidated. Everything else is cacheable.
    static bool isCacheable(const QUrl &url);

    std::optional<Entry> lookup(const QString &url) const;
    bool store(const QString &url, const QString &etag, const QString &lastModified, int status,
               const QByteArray &body);
    // Marks an entry as just used (a 304 revalidated it), so pruning by age
    // keeps entries that are still being asked for.
    void touch(const QString &url);

    // Drops expired and over-budget entries and returns the freed pages to
    // the filesystem. Called by open(); exposed for tests.
    void prune(int maxAgeDays, qint64 maxBytes);

    qint64 totalBodyBytes() const;
    int entryCount() const;

private:
    QString m_connectionName;
};

} // namespace pl::net
