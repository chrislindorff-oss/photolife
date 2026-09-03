#pragma once

#include <QObject>
#include <QString>

namespace pl::net {

class HttpClient;

// Optional, user-triggered check against the GitHub "latest release" API.
// No telemetry, no automatic polling — only runs when check() is called.
class UpdateChecker : public QObject
{
    Q_OBJECT

public:
    explicit UpdateChecker(HttpClient &http, QObject *parent = nullptr);

    void setRepo(const QString &ownerRepo) { m_repo = ownerRepo; }
    void setCurrentVersion(const QString &version) { m_current = version; }

    void check();

    // Exposed for testing: is `candidate` a newer release than `current`?
    // Unparseable `current` (a bare commit hash) counts every release as newer.
    static bool isNewer(const QString &candidate, const QString &current);

signals:
    void upToDate(const QString &currentVersion);
    void updateAvailable(const QString &latestVersion, const QString &releaseUrl);
    void checkFailed(const QString &error);

private:
    HttpClient &m_http;
    QString m_repo;
    QString m_current;
};

} // namespace pl::net
