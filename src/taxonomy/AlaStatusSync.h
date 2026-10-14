#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

#include <optional>

namespace pl::net {
class HttpClient;
}

namespace pl::taxonomy {

class TaxonomyStore;

// Conservation statuses from the Atlas of Living Australia: each state's and
// territory's authoritative conservation list, plus the national EPBC Act
// list, from ALA's species-lists service (api.ala.org.au/specieslist). Each
// list is recorded against its region's iNaturalist place (Victoria's list ->
// place 7830, EPBC -> Australia), so computeCoverage's regional rule counts it
// only in trees within that region.
//
// Names are matched against the local taxon cache (species and below,
// accepted names first, then synonyms) -- no per-name network lookups -- so
// run it again after a tree adds new taxa. Each list's statuses are replaced
// whole on every run (source "ALA <list id>").
//
// The downloaded lists are kept on disk (in `cacheDir`, shared by every
// catalogue) and downloaded again only when older than kMaxAgeDays or when
// forced; re-matching after a new tree -- or in a catalogue that hasn't run
// it yet -- uses the kept copy. With no `cacheDir` every run downloads.
// Downloads go at background priority unless forced (the user is then
// watching a progress window).
//
// Drive it from the thread that owns the HttpClient / store connection.
class AlaStatusSync : public QObject
{
    Q_OBJECT

public:
    static constexpr int kPageSize = 1000;
    static constexpr int kMaxAgeDays = 30;
    static const QString kBaseUrl;   // https://api.ala.org.au/specieslist/ws

    AlaStatusSync(net::HttpClient &http, TaxonomyStore &store, QString cacheDir = {},
                  QObject *parent = nullptr);

    void setBaseUrl(const QString &url) { m_baseUrl = url; }
    // Downloads the lists if the kept copy is missing, stale or
    // `forceDownload`, then matches them against the cached taxa.
    void start(bool forceDownload = false);
    // Stops after the request in flight; lists already saved stay saved.
    // finished() then reports ok = false with error "cancelled".
    void cancel() { m_cancelled = true; }
    bool isRunning() const { return m_running; }
    // Where the downloaded lists are kept; empty without a cacheDir.
    QString cachePath() const;

    // The iNaturalist place for an ALA list's region ("Victoria",
    // "Australia", ...), or nullopt for regions PhotoLife doesn't map.
    static std::optional<qint64> placeForRegion(const QString &region);

signals:
    // Starting list `index` (0-based) of `count`.
    void progress(int index, int count, const QString &listName);
    // `matched` statuses recorded across all lists.
    void finished(bool ok, const QString &error, int lists, int matched);

private:
    struct Item
    {
        QStringList names;   // ALA's matched name, then the name as listed
        QString status;
    };
    struct List
    {
        QString id;          // dataResourceUid, e.g. "dr655"
        QString name;
        qint64 placeInatId = 0;
        QList<Item> items;
    };

    bool loadCache();
    void saveCache() const;
    void fetchNextList();
    void fetchPage();
    void storeList();
    void fail(const QString &error);

    net::HttpClient &m_http;
    TaxonomyStore &m_store;
    QString m_cacheDir;
    QString m_baseUrl;
    bool m_running = false;
    bool m_cancelled = false;
    bool m_downloading = false;   // this run fetches the lists (else: from the kept copy)
    bool m_background = true;     // this run's requests are background priority
    bool checkCancelled();
    QList<List> m_lists;
    int m_listIndex = 0;
    int m_offset = 0;
    QHash<QString, qint64> m_nameIndex;
    int m_matched = 0;
};

} // namespace pl::taxonomy
