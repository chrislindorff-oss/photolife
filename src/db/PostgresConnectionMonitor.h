#pragma once

#include <QObject>
#include <QString>
#include <QTimer>

namespace pl {

// Periodically pings a QSqlDatabase connection with a cheap query and
// reports whether it's actually alive. QSqlDatabase::isOpen() only reflects
// local driver state, not whether the network connection is still there or
// a managed instance (e.g. Neon's free-tier autosuspend) has gone to sleep
// -- this catches both by actually round-tripping to the server. Works
// against any backend (SQLite included, for testing); callers decide
// whether it's worth running at all for a given connection.
class PostgresConnectionMonitor : public QObject
{
    Q_OBJECT

public:
    explicit PostgresConnectionMonitor(QString connectionName, QObject *parent = nullptr);

    // Runs an immediate check, then repeats every `intervalMs`.
    void start(int intervalMs = 30000);

public slots:
    // Runs one check right now, outside the regular interval.
    void checkNow();

signals:
    // Emitted only when the connected/disconnected state actually changes,
    // not on every check -- the first check always emits once, whatever it
    // finds.
    void connectedChanged(bool connected);

private:
    QString m_connectionName;
    QTimer m_timer;
    bool m_connected = false;
    bool m_everChecked = false;
};

} // namespace pl
