#include "db/PostgresConnectionMonitor.h"

#include <QSqlDatabase>
#include <QSqlQuery>

namespace pl {

PostgresConnectionMonitor::PostgresConnectionMonitor(QString connectionName, QObject *parent)
    : QObject(parent), m_connectionName(std::move(connectionName))
{
    connect(&m_timer, &QTimer::timeout, this, &PostgresConnectionMonitor::checkNow);
}

void PostgresConnectionMonitor::start(int intervalMs)
{
    checkNow();
    m_timer.start(intervalMs);
}

void PostgresConnectionMonitor::checkNow()
{
    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    bool ok = db.isValid() && db.isOpen();
    if (ok) {
        QSqlQuery q(db);
        ok = q.exec(QStringLiteral("SELECT 1"));
    }

    if (!m_everChecked || ok != m_connected) {
        m_connected = ok;
        m_everChecked = true;
        emit connectedChanged(ok);
    }
}

} // namespace pl
