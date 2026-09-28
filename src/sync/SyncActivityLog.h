#pragma once

#include <QDateTime>
#include <QList>
#include <QString>

namespace pl::sync {

// One server action on this computer: Start Editing, Sync, a lost
// connection ... for the Activity Log window.
struct SyncActivityEntry
{
    QDateTime at;
    QString action;       // "Sync", "Start Editing", "Connection lost" ...
    bool ok = true;
    QString detail;       // "Sent 12 changes", "Got version 14" ...
    QString error;
    qint64 durationMs = 0;
};

// The activity log: JSON lines appended to a file in the data folder, so it
// survives restarts (and the restarts sync itself causes).
class SyncActivityLog
{
public:
    explicit SyncActivityLog(QString path) : m_path(std::move(path)) {}

    QString path() const { return m_path; }
    bool append(const SyncActivityEntry &entry) const;
    // Newest first, at most `limit`.
    QList<SyncActivityEntry> read(int limit = 2000) const;

private:
    QString m_path;
};

} // namespace pl::sync
