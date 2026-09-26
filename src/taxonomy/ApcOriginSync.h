#pragma once

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QString>
#include <QUrl>

#include <optional>

class QIODevice;
class QNetworkAccessManager;

namespace pl::taxonomy {

class TaxonomyStore;

// Whether plants are native or introduced in each Australian state and
// territory, from the Australian Plant Census (APC) -- the national plant
// name service's full taxon export (biodiversity.org.au/nsl, ~100 MB CSV).
// Each accepted taxon's taxonDistribution ("WA (naturalised), SA, Vic
// (native and naturalised), ...") becomes one taxon_origin row per state
// (source "APC"), recorded against the state's iNaturalist place so the usual
// regional rule applies. Synonyms carry their accepted taxon's distribution,
// so iNaturalist names APC treats as synonyms still match.
//
// The CSV is kept on disk and re-downloaded only when older than maxAgeDays
// (or when forced); re-matching after a new tree uses the kept copy. Parsing
// runs on a worker thread; matching and writing on the calling thread, which
// must own the store's connection.
class ApcOriginSync : public QObject
{
    Q_OBJECT

public:
    static const QUrl kExportUrl;
    static constexpr int kMaxAgeDays = 60;

    ApcOriginSync(TaxonomyStore &store, QString cacheDir, QByteArray userAgent,
                  QObject *parent = nullptr);

    void setDownloadUrl(const QUrl &url) { m_url = url; }
    // Downloads the CSV if it's missing, stale or `forceDownload`, then
    // matches it against the cached taxa.
    void start(bool forceDownload = false);
    bool isRunning() const { return m_running; }
    QString csvPath() const;

    // "introduced" / "native" per state code ("Vic", "NSW", ...) in one
    // APC taxonDistribution value. Offshore islands and "uncertain origin"
    // entries are left out.
    static QHash<QString, QString> parseDistribution(const QString &distribution);
    // The iNaturalist place for an APC state/territory code.
    static std::optional<qint64> placeForState(const QString &code);
    // Folded name (TaxonomyStore::foldListName) -> taxonDistribution, for
    // every accepted taxon and its synonyms, from the APC taxon CSV.
    static QHash<QString, QString> readTaxonCsv(QIODevice &device);

signals:
    void progress(const QString &phase);
    // `matched` taxa given an origin.
    void finished(bool ok, const QString &error, int matched);

private:
    void download();
    void parseAndStore();
    void fail(const QString &error);

    TaxonomyStore &m_store;
    QString m_cacheDir;
    QByteArray m_userAgent;
    QUrl m_url;
    QNetworkAccessManager *m_nam = nullptr;
    bool m_running = false;
};

} // namespace pl::taxonomy
