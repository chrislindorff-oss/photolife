#include "lightroom/LightroomCatalogReader.h"

#include <QFileInfo>
#include <QHash>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>

#include <atomic>

namespace pl::lightroom {
namespace {

int nextConnectionSuffix()
{
    static std::atomic_int counter{0};
    return counter.fetch_add(1);
}

// Required tables and, per table, the columns this reader relies on. Checked
// defensively (PRAGMA table_info) before the join query runs -- see the
// class comment on the schema not being verified against a real catalog.
struct RequiredTable
{
    QString name;
    QStringList columns;
};

const QList<RequiredTable> &requiredTables()
{
    static const QList<RequiredTable> tables = {
        {QStringLiteral("Adobe_images"), {QStringLiteral("id_local"), QStringLiteral("rootFile")}},
        {QStringLiteral("AgLibraryFile"),
         {QStringLiteral("id_local"), QStringLiteral("folder"), QStringLiteral("baseName"),
          QStringLiteral("extension")}},
        {QStringLiteral("AgLibraryFolder"),
         {QStringLiteral("id_local"), QStringLiteral("rootFolder"),
          QStringLiteral("pathFromRoot")}},
        {QStringLiteral("AgLibraryRootFolder"),
         {QStringLiteral("id_local"), QStringLiteral("absolutePath")}},
        {QStringLiteral("AgLibraryKeyword"), {QStringLiteral("id_local"), QStringLiteral("name")}},
        {QStringLiteral("AgLibraryKeywordImage"), {QStringLiteral("image"), QStringLiteral("tag")}},
    };
    return tables;
}

QStringList existingColumns(QSqlDatabase &db, const QString &table)
{
    QStringList columns;
    QSqlQuery q(db);
    // PRAGMA doesn't take bind parameters; `table` only ever comes from the
    // fixed list above, never from the catalog file itself.
    if (!q.exec(QStringLiteral("PRAGMA table_info(%1)").arg(table)))
        return columns;
    while (q.next())
        columns << q.value(1).toString();
    return columns;
}

// Joins two path segments, tolerating either side missing its trailing/
// leading slash -- Lightroom isn't consistent about it between
// AgLibraryRootFolder.absolutePath and AgLibraryFolder.pathFromRoot.
QString joinPath(const QString &a, const QString &b)
{
    if (a.isEmpty())
        return b;
    if (b.isEmpty())
        return a;
    const bool aSlash = a.endsWith(QLatin1Char('/')) || a.endsWith(QLatin1Char('\\'));
    const bool bSlash = b.startsWith(QLatin1Char('/')) || b.startsWith(QLatin1Char('\\'));
    if (aSlash && bSlash)
        return a + b.mid(1);
    if (!aSlash && !bSlash)
        return a + QLatin1Char('/') + b;
    return a + b;
}

} // namespace

QList<LightroomPhotoKeywords> LightroomCatalogReader::read(const QString &lrcatPath)
{
    m_error.clear();

    if (!QFileInfo::exists(lrcatPath)) {
        m_error = QStringLiteral("%1 does not exist").arg(lrcatPath);
        return {};
    }

    const QString connectionName =
        QStringLiteral("photolife-lrcat-%1").arg(nextConnectionSuffix());
    QList<LightroomPhotoKeywords> result;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
        db.setDatabaseName(lrcatPath);
        db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
        if (!db.open()) {
            m_error = QStringLiteral("cannot open %1: %2").arg(lrcatPath, db.lastError().text());
            QSqlDatabase::removeDatabase(connectionName);
            return {};
        }

        for (const RequiredTable &table : requiredTables()) {
            const QStringList columns = existingColumns(db, table.name);
            if (columns.isEmpty()) {
                m_error = QStringLiteral(
                    "%1 does not look like a Lightroom catalog (missing table %2)")
                              .arg(lrcatPath, table.name);
                break;
            }
            for (const QString &column : table.columns) {
                if (!columns.contains(column)) {
                    m_error = QStringLiteral(
                        "%1 does not look like a Lightroom catalog (missing %2.%3)")
                                  .arg(lrcatPath, table.name, column);
                    break;
                }
            }
            if (!m_error.isEmpty())
                break;
        }

        if (m_error.isEmpty()) {
            QSqlQuery q(db);
            q.setForwardOnly(true);
            const bool ran = q.exec(QStringLiteral(
                "SELECT ai.id_local, rf.absolutePath, f.pathFromRoot, "
                "  file.baseName, file.extension, kw.name "
                "FROM Adobe_images ai "
                "JOIN AgLibraryFile file ON file.id_local = ai.rootFile "
                "JOIN AgLibraryFolder f ON f.id_local = file.folder "
                "JOIN AgLibraryRootFolder rf ON rf.id_local = f.rootFolder "
                "JOIN AgLibraryKeywordImage ki ON ki.image = ai.id_local "
                "JOIN AgLibraryKeyword kw ON kw.id_local = ki.tag "
                "WHERE kw.name IS NOT NULL AND kw.name != ''"));
            if (!ran) {
                m_error = QStringLiteral("query failed against %1: %2")
                              .arg(lrcatPath, q.lastError().text());
            } else {
                QHash<qint64, int> indexByImage;
                while (q.next()) {
                    const qint64 imageId = q.value(0).toLongLong();
                    const QString rootPath = q.value(1).toString();
                    const QString folderPath = q.value(2).toString();
                    const QString baseName = q.value(3).toString();
                    const QString extension = q.value(4).toString();
                    const QString keyword = q.value(5).toString().trimmed();
                    if (keyword.isEmpty())
                        continue;

                    const QString filename = extension.isEmpty()
                                                  ? baseName
                                                  : baseName + QLatin1Char('.') + extension;
                    const QString absolutePath =
                        joinPath(joinPath(rootPath, folderPath), filename);

                    auto it = indexByImage.find(imageId);
                    if (it == indexByImage.end()) {
                        LightroomPhotoKeywords photo;
                        photo.absolutePath = absolutePath;
                        result.append(photo);
                        it = indexByImage.insert(imageId, result.size() - 1);
                    }
                    QStringList &keywords = result[it.value()].keywords;
                    if (!keywords.contains(keyword))
                        keywords.append(keyword);
                }
            }
        }

        db.close();
    }
    QSqlDatabase::removeDatabase(connectionName);

    if (!m_error.isEmpty())
        return {};
    return result;
}

} // namespace pl::lightroom
