#pragma once

#include "inat/DuplicateFinder.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QPair>
#include <QString>

namespace pl::net {
class INatClient;
}

namespace pl::inat {

// Looks for proposed observations among the user's own: first the library
// (photos already on an observation, no network), then iNaturalist itself,
// over the days the proposed ones were seen -- so a sighting posted from
// the iNaturalist app or website isn't uploaded again. Observations with
// and without photos both count. The answer comes once, from finished().
class InatDuplicateCheck : public QObject
{
    Q_OBJECT

public:
    struct Group
    {
        int key = 0;   // what the caller calls it
        ProposedObservation observation;
    };

    InatDuplicateCheck(net::INatClient &inat, QString connectionName, QObject *parent = nullptr);

    // `userLogin` empty: the library only.
    void start(const QString &userLogin, const QList<Group> &groups);
    void cancel() { m_cancelled = true; }
    bool isRunning() const { return m_running; }

    // The user's observations the library knows about, seen in these date
    // ranges: one per observation, from its photos. Exposed for tests.
    static QList<ExistingObservation> fromLibrary(const QString &connectionName,
                                                  const QList<QPair<QDate, QDate>> &ranges);

    static constexpr int kPerPage = 200;
    static constexpr int kMaxPages = 25;   // per range: 5,000 observations

signals:
    void progress(int done, int total);
    // `hits`: each group's (by key) that has any. `ok` false when
    // iNaturalist couldn't be asked; the library's hits are there anyway.
    void finished(bool ok, const QString &error, int status, const QHash<int, QList<pl::inat::DuplicateHit>> &hits);

private:
    void fetchPage();
    void finish(bool ok, const QString &error, int status);

    net::INatClient &m_inat;
    QString m_connectionName;
    QString m_login;
    QList<Group> m_groups;
    QList<QPair<QDate, QDate>> m_ranges;
    int m_range = 0;
    int m_page = 1;
    QList<ExistingObservation> m_existing;
    bool m_running = false;
    bool m_cancelled = false;
};

} // namespace pl::inat
