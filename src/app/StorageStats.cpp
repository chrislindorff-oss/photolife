#include "app/StorageStats.h"

#include <QDir>
#include <QDirIterator>

namespace pl::app {

qint64 directoryBytes(const QString &dir)
{
    qint64 total = 0;
    QDirIterator it(dir, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        total += it.fileInfo().size();
    }
    return total;
}

} // namespace pl::app
