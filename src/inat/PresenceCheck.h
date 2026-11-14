#pragma once

#include <QObject>
#include <QString>

namespace pl::net {
class INatClient;
}

namespace pl::inat {

// Runs "Already on iNaturalist?": reads the details of the user's
// observations from iNaturalist into the cache (only those newer than the
// cache, or all of them with `rereadAll`), then compares every named photo
// not known to be on iNaturalist with them (PresenceMatcher). High matches
// are marked on iNaturalist and linked; Medium and Low are left as "maybe"
// for the user. Read-only towards iNaturalist. Drive it from the thread that
// owns the INatClient and the catalogue connection.
class PresenceCheck : public QObject
{
    Q_OBJECT

public:
    struct Result
    {
        bool ok = false;
        QString error;
        int status = 0;          // HTTP status when reading failed (401: token expired)
        bool cancelled = false;
        int observationsRead = 0;   // this run
        int observationsKnown = 0;  // in the cache afterwards
        int photosChecked = 0;
        int marked = 0;             // High: marked on iNaturalist
        int medium = 0;             // to check
        int low = 0;
        QString run;                // this run's id, for Undo
    };

    PresenceCheck(net::INatClient &inat, QString connectionName, QObject *parent = nullptr);

    void start(const QString &login, bool rereadAll = false);
    void cancel();
    bool isRunning() const { return m_running; }

    // The comparing and recording on their own (no network): what start()
    // does once the cache is up to date. Exposed for tests.
    static Result checkAgainstCache(const QString &connectionName, const QString &login);

signals:
    // `stage`: what it's doing ("Reading your observations…").
    void progress(const QString &stage, int done, int total);
    void finished(const pl::inat::PresenceCheck::Result &result);

private:
    void fetchNext();
    void finish(Result result);

    net::INatClient &m_inat;
    QString m_connectionName;
    QString m_login;
    qint64 m_idAbove = 0;
    int m_read = 0;
    bool m_running = false;
    bool m_cancelled = false;
};

} // namespace pl::inat

Q_DECLARE_METATYPE(pl::inat::PresenceCheck::Result)
