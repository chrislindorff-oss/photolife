#include "scan/UnsortedFolders.h"

#include "raw/RawPreview.h"
#include "scan/Exif.h"
#include "scan/FilenameParser.h"

#include <QDir>
#include <QHash>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>

#include <algorithm>

namespace pl::scan {

QList<QList<int>> groupRuns(const QList<RunItem> &items, int gapSeconds)
{
    // By group, then time (untimed ones by day, after the timed).
    QList<int> order(items.size());
    for (int i = 0; i < items.size(); ++i)
        order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        const RunItem &x = items.at(a), &y = items.at(b);
        if (x.group != y.group)
            return x.group < y.group;
        if (x.taken.isValid() != y.taken.isValid())
            return x.taken.isValid();
        if (x.taken.isValid())
            return x.taken < y.taken;
        return x.day < y.day;
    });
    QList<QList<int>> runs;
    for (int i : std::as_const(order)) {
        const RunItem &it = items.at(i);
        bool joins = false;
        if (!runs.isEmpty()) {
            const RunItem &last = items.at(runs.last().last());
            if (last.group == it.group) {
                if (it.taken.isValid() && last.taken.isValid())
                    joins = last.taken.secsTo(it.taken) <= gapSeconds;
                else if (!it.taken.isValid() && !last.taken.isValid())
                    joins = last.day == it.day;   // both undated: one run
            }
        }
        if (joins)
            runs.last() << i;
        else
            runs << QList<int>{i};
    }
    return runs;
}

namespace Unsorted {

namespace {

QSqlDatabase dbFor(const QString &connectionName)
{
    return QSqlDatabase::database(connectionName, false);
}

QString clean(const QString &path)
{
    return QDir::cleanPath(path);
}

// SQL: `column` (a folder path) is `folder` or below it. substr rather than
// LIKE: folder names have _ and % in them ("____2026").
QString underClause(const QString &column)
{
    return QStringLiteral("(%1 = ? OR substr(%1, 1, ?) = ?)").arg(column);
}

void bindUnder(QSqlQuery &q, const QString &folder)
{
    const QString prefix = folder + QLatin1Char('/');
    q.addBindValue(folder);
    q.addBindValue(int(prefix.size()));
    q.addBindValue(prefix);
}

} // namespace

QStringList folders(const QString &connectionName)
{
    QStringList out;
    QSqlQuery q(dbFor(connectionName));
    if (q.exec(QStringLiteral("SELECT path FROM unsorted_folder ORDER BY path")))
        while (q.next())
            out << q.value(0).toString();
    return out;
}

bool contains(const QStringList &folders, const QString &path)
{
    const QString p = clean(path);
    for (const QString &f : folders)
        if (p == f || p.startsWith(f + QLatin1Char('/')))
            return true;
    return false;
}

QString groupKey(const QString &dirPath, const QString &baseName)
{
    QString name = parseStem(baseName).name;
    if (name.isEmpty() || isCameraFileName(name))
        name.clear();
    return clean(dirPath) + QLatin1Char('|') + name.toLower();
}

QDateTime fileTime(const DiscoveredFile &file)
{
    if (file.isRaw)
        return raw::readInfo(file.path).taken;
    if (file.ext == QLatin1String("jpg") || file.ext == QLatin1String("jpeg") || file.ext == QLatin1String("jpe"))
        return readJpegExif(file.path).dateTimeOriginal;
    return {};
}

ThinResult mark(const QString &connectionName, const QString &folderIn)
{
    ThinResult r;
    const QString folder = clean(folderIn);
    QSqlDatabase db = dbFor(connectionName);
    if (!db.transaction()) {
        r.error = db.lastError().text();
        return r;
    }
    auto fail = [&](const QSqlQuery &q) {
        r.error = q.lastError().text();
        db.rollback();
        return r;
    };
    {
        QSqlQuery ins(db);
        ins.prepare(QStringLiteral("DELETE FROM unsorted_folder WHERE path = ?"));
        ins.addBindValue(folder);
        ins.exec();
        ins.prepare(QStringLiteral("INSERT INTO unsorted_folder (path, marked_at) VALUES (?, ?)"));
        ins.addBindValue(folder);
        ins.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
        if (!ins.exec())
            return fail(ins);
    }

    // The photos there, with whether you've decided anything about them.
    struct Photo
    {
        int id = 0;
        int folderId = 0;
        QString baseName;
        QString capturedOn;
        bool decided = false;
    };
    QList<Photo> photos;
    QList<RunItem> items;
    {
        QSqlQuery q(db);
        q.prepare(QStringLiteral(
                      "SELECT c.id, c.folder_id, f.path, c.base_name, c.captured_on, "
                      "  (EXISTS (SELECT 1 FROM capture_match m WHERE m.capture_id = c.id AND m.decided_by = 'user') "
                      "   OR EXISTS (SELECT 1 FROM best_shot b WHERE b.capture_id = c.id) "
                      "   OR EXISTS (SELECT 1 FROM representative p WHERE p.capture_id = c.id) "
                      "   OR EXISTS (SELECT 1 FROM capture_inat_status s WHERE s.capture_id = c.id "
                      "              AND s.decided_by = 'user') "
                      "   OR c.inat_observation_id IS NOT NULL), "
                      "  c.time_checked, "
                      "  (SELECT r.path FROM rendition r WHERE r.capture_id = c.id ORDER BY r.id LIMIT 1), "
                      "  (SELECT r.kind FROM rendition r WHERE r.capture_id = c.id ORDER BY r.id LIMIT 1), "
                      "  (SELECT r.ext FROM rendition r WHERE r.capture_id = c.id ORDER BY r.id LIMIT 1) "
                      "FROM capture c JOIN folder f ON f.id = c.folder_id WHERE %1")
                      .arg(underClause(QStringLiteral("f.path"))));
        bindUnder(q, folder);
        if (!q.exec())
            return fail(q);
        while (q.next()) {
            Photo p;
            p.id = q.value(0).toInt();
            p.folderId = q.value(1).toInt();
            p.baseName = q.value(3).toString();
            p.capturedOn = q.value(4).toString();
            p.decided = q.value(5).toBool();
            RunItem item;
            item.group = groupKey(q.value(2).toString(), p.baseName);
            if (p.capturedOn.size() > 10)
                item.taken = QDateTime::fromString(p.capturedOn.left(19), Qt::ISODate);
            else if (q.value(6).toInt() == 0 && !q.value(7).toString().isEmpty()) {
                // Its camera time not looked for yet (a RAW from before):
                // read it now, or the run would be the whole day.
                DiscoveredFile file;
                file.path = q.value(7).toString();
                file.isRaw = q.value(8).toString() == QLatin1String("raw");
                file.ext = q.value(9).toString();
                const QDateTime t = fileTime(file);
                if (t.isValid() && (p.capturedOn.isEmpty() || t.date() == QDate::fromString(p.capturedOn, Qt::ISODate))) {
                    item.taken = t;
                    p.capturedOn = t.toString(Qt::ISODate);
                }
            }
            item.day = QDate::fromString(p.capturedOn.left(10), Qt::ISODate);
            photos << p;
            items << item;
        }
    }
    r.photos = int(photos.size());

    const QList<QList<int>> runs = groupRuns(items);
    r.runs = int(runs.size());
    QList<int> drop;   // capture ids no longer catalogued
    for (const QList<int> &run : runs) {
        // The representative: one you've decided on, else the middle one.
        int rep = run.at(run.size() / 2);
        for (int i : run)
            if (photos.at(i).decided) {
                rep = i;
                break;
            }
        for (int i : run) {
            const Photo &p = photos.at(i);
            if (i == rep || p.decided) {
                ++r.kept;
                continue;
            }
            // Remembered, each of its files, as part of the representative's run.
            QSqlQuery files(db);
            files.prepare(QStringLiteral("SELECT path, file_size, mtime FROM rendition WHERE capture_id = ?"));
            files.addBindValue(p.id);
            if (!files.exec())
                return fail(files);
            while (files.next()) {
                QSqlQuery ins(db);
                ins.prepare(QStringLiteral("DELETE FROM unsorted_photo WHERE path = ?"));
                ins.addBindValue(files.value(0));
                ins.exec();
                ins.prepare(QStringLiteral(
                    "INSERT INTO unsorted_photo (path, folder_id, base_name, file_size, mtime, taken, capture_id) "
                    "VALUES (?, ?, ?, ?, ?, ?, ?)"));
                ins.addBindValue(files.value(0));
                ins.addBindValue(p.folderId);
                ins.addBindValue(p.baseName);
                ins.addBindValue(files.value(1).toLongLong());
                ins.addBindValue(files.value(2).toLongLong());
                ins.addBindValue(p.capturedOn.isEmpty() ? QVariant() : p.capturedOn);
                ins.addBindValue(photos.at(rep).id);
                if (!ins.exec())
                    return fail(ins);
            }
            drop << p.id;
        }
    }
    // Their catalogue entries go (rendition and match rows with them); the
    // files are untouched.
    for (qsizetype from = 0; from < drop.size(); from += 500) {
        QStringList ids;
        for (int id : drop.mid(from, 500))
            ids << QString::number(id);
        QSqlQuery del(db);
        if (!del.exec(QStringLiteral("DELETE FROM capture WHERE id IN (%1)").arg(ids.join(QLatin1Char(',')))))
            return fail(del);
    }
    r.removed = int(drop.size());
    if (!db.commit()) {
        r.error = db.lastError().text();
        return r;
    }
    r.ok = true;
    return r;
}

bool unmark(const QString &connectionName, const QString &folderIn)
{
    const QString folder = clean(folderIn);
    QSqlDatabase db = dbFor(connectionName);
    if (!db.transaction())
        return false;
    QSqlQuery q(db);
    q.prepare(QStringLiteral("DELETE FROM unsorted_folder WHERE path = ?"));
    q.addBindValue(folder);
    bool ok = q.exec();
    // Its remembered photos, unless another Unsorted folder above still covers them.
    const QStringList others = folders(connectionName);
    if (ok && !contains(others, folder)) {
        QSqlQuery del(db);
        del.prepare(QStringLiteral("DELETE FROM unsorted_photo WHERE folder_id IN (SELECT id FROM folder f WHERE %1)")
                        .arg(underClause(QStringLiteral("f.path"))));
        bindUnder(del, folder);
        ok = del.exec();
    }
    if (!ok) {
        db.rollback();
        return false;
    }
    return db.commit();
}

QList<FolderStats> stats(const QString &connectionName)
{
    QList<FolderStats> out;
    for (const QString &folder : folders(connectionName)) {
        FolderStats s;
        s.path = folder;
        QSqlQuery q(dbFor(connectionName));
        q.prepare(QStringLiteral("SELECT COUNT(*) FROM capture c JOIN folder f ON f.id = c.folder_id WHERE %1")
                      .arg(underClause(QStringLiteral("f.path"))));
        bindUnder(q, folder);
        if (q.exec() && q.next())
            s.catalogued = q.value(0).toInt();
        q.prepare(QStringLiteral("SELECT COUNT(DISTINCT u.folder_id || '/' || u.base_name) FROM unsorted_photo u "
                                 "JOIN folder f ON f.id = u.folder_id WHERE %1")
                      .arg(underClause(QStringLiteral("f.path"))));
        bindUnder(q, folder);
        if (q.exec() && q.next())
            s.remembered = q.value(0).toInt();
        out << s;
    }
    return out;
}

QSet<qint64> unsortedOnlyTaxa(const QString &connectionName)
{
    QSet<qint64> out;
    const QStringList marked = folders(connectionName);
    if (marked.isEmpty())
        return out;
    QSqlQuery q(dbFor(connectionName));
    q.setForwardOnly(true);
    if (!q.exec(QStringLiteral(
            "SELECT t.inat_id, f.path FROM capture c JOIN folder f ON f.id = c.folder_id "
            "JOIN capture_match m ON m.id = (SELECT id FROM capture_match WHERE capture_id = c.id "
            "  ORDER BY (decided_by = 'user') DESC, confidence DESC LIMIT 1) "
            "JOIN taxon t ON t.id = m.taxon_id WHERE m.status IN ('auto', 'confirmed')")))
        return out;
    QSet<qint64> sorted;
    while (q.next()) {
        const qint64 id = q.value(0).toLongLong();
        if (contains(marked, q.value(1).toString()))
            out.insert(id);
        else
            sorted.insert(id);
    }
    return out - sorted;
}

} // namespace Unsorted

} // namespace pl::scan
