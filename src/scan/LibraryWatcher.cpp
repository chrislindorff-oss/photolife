#include "scan/LibraryWatcher.h"

#include "db/Database.h"

#include <QDir>
#include <QLoggingCategory>
#include <QSqlDatabase>
#include <QSqlQuery>

namespace pl::scan {

LibraryWatcher::LibraryWatcher(pl::Database &db, QObject *parent)
    : QObject(parent), m_db(db)
{
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(1500);
    connect(&m_debounce, &QTimer::timeout, this, &LibraryWatcher::changeDetected);

    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, &LibraryWatcher::onChanged);
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, &LibraryWatcher::onChanged);
}

void LibraryWatcher::setRoots(const QStringList &roots)
{
    m_roots = roots;
    refresh();
}

void LibraryWatcher::setDebounceInterval(int milliseconds)
{
    m_debounce.setInterval(milliseconds);
}

void LibraryWatcher::onChanged(const QString &)
{
    m_debounce.start();
}

void LibraryWatcher::refresh()
{
    if (!m_watcher.directories().isEmpty())
        m_watcher.removePaths(m_watcher.directories());
    if (!m_watcher.files().isEmpty())
        m_watcher.removePaths(m_watcher.files());

    QStringList paths;
    for (const QString &root : m_roots) {
        const QString clean = QDir::cleanPath(root);
        if (QDir(clean).exists())
            paths << clean;
    }

    if (m_db.isOpen()) {
        QSqlQuery q(QSqlDatabase::database(m_db.connectionName(), false));
        q.setForwardOnly(true);
        if (q.exec(QStringLiteral("SELECT path FROM folder"))) {
            while (q.next()) {
                const QString path = q.value(0).toString();
                if (QDir(path).exists())
                    paths << path;
            }
        }
    }

    paths.removeDuplicates();
    if (paths.isEmpty())
        return;

    const QStringList failed = m_watcher.addPaths(paths);
    if (!failed.isEmpty()) {
        qWarning("LibraryWatcher: %lld of %lld folders could not be watched "
                 "(likely the OS watch limit); changes there need a manual rescan",
                 static_cast<long long>(failed.size()),
                 static_cast<long long>(paths.size()));
    }
}

} // namespace pl::scan
