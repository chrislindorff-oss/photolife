#pragma once

#include <QList>
#include <QObject>
#include <QString>

namespace pl::net {
class INatClient;
}

namespace pl::taxonomy {

class TaxonomyStore;

// Fetches every English common name iNaturalist has for the taxa of all the
// reference trees -- not just its one preferred name -- so photos filed under
// another name ("Purple Swamphen" for the Australasian Swamphen) still match.
// Batches of 30 through GET /v1/taxa/{ids}?all_names=true; each reply takes
// iNaturalist several seconds, so a big library's trees take hours. The taxa
// photos are matched to go first (Scope::PhotoTaxa, minutes), then the rest
// most useful first (see TaxonomyStore::treeTaxaByNamePriority()).
//
// Which taxa were fetched, and when, is kept in a small file of its own
// (beside the catalogue, not in it): a taxon is fetched again only after
// kRefreshDays, so it's cheap to run after every tree build, and an
// interrupted run carries on where it stopped.
//
// Drive it from the thread that owns the INatClient / store connection.
class CommonNameFetcher : public QObject
{
    Q_OBJECT

public:
    static constexpr int kRefreshDays = 90;

    enum class Scope {
        PhotoTaxa,   // the taxa photos are matched to
        All,         // every tree taxon, photos' first
    };

    CommonNameFetcher(pl::net::INatClient &inat, TaxonomyStore &store, QString recordPath,
                      QObject *parent = nullptr);
    ~CommonNameFetcher() override;

    // The record kept beside the catalogue at `databasePath`.
    static QString recordPathFor(const QString &databasePath);

    // With `force`, fetches them again even if fetched lately.
    void start(Scope scope = Scope::All, bool force = false);
    void cancel() { m_cancelled = true; }
    bool isRunning() const { return m_running; }

    // The taxa (iNat ids) still to fetch, in fetching order.
    QList<qint64> due(Scope scope = Scope::All, bool force = false);
    Scope scope() const { return m_scope; }

signals:
    void progress(int done, int total);
    // `added`: the common names the cache gained.
    void finished(bool ok, const QString &error, int added);

private:
    bool openRecord();
    void fetchNext();
    void finish(bool ok, const QString &error);
    bool checkCancelled();

    pl::net::INatClient &m_inat;
    TaxonomyStore &m_store;
    QString m_recordPath;
    QString m_connection;

    Scope m_scope = Scope::All;
    bool m_running = false;
    bool m_cancelled = false;
    QList<QList<qint64>> m_batches;
    int m_index = 0;
    int m_done = 0;
    int m_total = 0;
    int m_updated = 0;
};

} // namespace pl::taxonomy
