#pragma once

#include <QFileSystemWatcher>
#include <QObject>
#include <QStringList>
#include <QTimer>

namespace pl {
class Database;
}

namespace pl::scan {

// Watches the catalogued folders for changes and raises a single debounced
// signal so the caller can trigger an incremental rescan. QFileSystemWatcher
// does not recurse, so the watch list is the set of folders recorded by the
// last scan (from the `folder` table) plus the configured roots.
class LibraryWatcher : public QObject
{
    Q_OBJECT

public:
    explicit LibraryWatcher(pl::Database &db, QObject *parent = nullptr);

    void setRoots(const QStringList &roots);
    void setDebounceInterval(int milliseconds);

    // Rebuilds the watch list from the roots and the `folder` table. Call after
    // each scan so newly-created folders are watched too.
    void refresh();

    int watchedDirectoryCount() const { return m_watcher.directories().size(); }

signals:
    void changeDetected();

private:
    void onChanged(const QString &path);

    pl::Database &m_db;
    QStringList m_roots;
    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
};

} // namespace pl::scan
