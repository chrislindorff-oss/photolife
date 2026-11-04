#include "scan/LibraryRenamer.h"

#include "scan/CaptureRenamer.h"
#include "util/PathSanitize.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include <algorithm>

namespace pl::scan {

using collection::FileNamePattern;
using collection::NameToken;

namespace {

// A file of the folder that goes by `stem`: named it, or it and an extension.
bool isNamed(const QString &entry, const QString &stem)
{
    return entry.compare(stem, Qt::CaseInsensitive) == 0
           || entry.startsWith(stem + QLatin1Char('.'), Qt::CaseInsensitive);
}

} // namespace

QString LibraryRenamer::cleanName(const QString &name)
{
    return CaptureRenamer::cleanStem(name);
}

QString LibraryRenamer::speciesName(const QString &scientific, const QString &common, NameForm form)
{
    if (scientific.isEmpty())
        return {};
    const bool hasCommon = !common.isEmpty() && common != scientific;
    switch (form) {
    case NameForm::Typed:
        return {};
    case NameForm::Common:
        return hasCommon ? common : scientific;
    case NameForm::Scientific:
        return scientific;
    case NameForm::CommonScientific:
        return hasCommon ? QStringLiteral("%1 (%2)").arg(common, scientific) : scientific;
    case NameForm::ScientificCommon:
        return hasCommon ? QStringLiteral("%1 (%2)").arg(scientific, common) : scientific;
    }
    return {};
}

QList<LibraryRenamer::Photo> LibraryRenamer::load(const QString &connectionName, const QList<qint64> &captureIds)
{
    QSqlDatabase db = QSqlDatabase::database(connectionName, false);
    QList<Photo> photos;
    for (qint64 id : captureIds) {
        QSqlQuery q(db);
        q.prepare(QStringLiteral(
            "SELECT c.folder_id, f.path, c.base_name, c.name_text, c.captured_on, c.locality_text, "
            "       t.name, t.common_name, g.locality "
            "FROM capture c JOIN folder f ON f.id = c.folder_id "
            "LEFT JOIN capture_match m ON m.id = ("
            "   SELECT id FROM capture_match WHERE capture_id = c.id AND status IN ('confirmed', 'auto') "
            "   ORDER BY (decided_by = 'user') DESC, confidence DESC LIMIT 1) "
            "LEFT JOIN taxon t ON t.id = m.taxon_id "
            "LEFT JOIN geocode_cache g ON g.lat_round = ROUND(CAST(c.latitude AS NUMERIC), 3) "
            "   AND g.lon_round = ROUND(CAST(c.longitude AS NUMERIC), 3) "
            "WHERE c.id = ?"));
        q.addBindValue(id);
        if (!q.exec() || !q.next())
            continue;
        Photo p;
        p.captureId = id;
        p.folderId = q.value(0).toInt();
        p.folder = q.value(1).toString();
        p.oldStem = q.value(2).toString();
        p.nameText = q.value(3).toString();
        p.scientificName = q.value(6).toString();
        p.commonName = q.value(7).toString();
        p.fields.scientificName = p.scientificName;
        if (p.commonName != p.scientificName)
            p.fields.commonName = p.commonName;
        p.fields.captured = collection::parseCapturedOn(q.value(4).toString());
        p.fields.fileLocality = q.value(5).toString();
        const collection::LocalityParts place = collection::splitLocality(q.value(8).toString());
        p.fields.town = place.town;
        p.fields.state = place.state;
        p.fields.country = place.country;
        p.fields.originalStem = p.oldStem;

        QSqlQuery r(db);
        r.prepare(QStringLiteral("SELECT path FROM rendition WHERE capture_id = ? ORDER BY (kind = 'raw'), id"));
        r.addBindValue(id);
        bool any = false;
        if (r.exec()) {
            while (r.next()) {
                const QString path = r.value(0).toString();
                if (!any)
                    p.suffix = QFileInfo(path).suffix();
                any = true;
                p.onDisk = p.onDisk && QFileInfo::exists(path);
            }
        }
        p.onDisk = p.onDisk && any;
        photos << p;
    }
    // By folder, then when taken (undated last): the order sequence numbers count in.
    std::stable_sort(photos.begin(), photos.end(), [](const Photo &a, const Photo &b) {
        if (a.folder != b.folder)
            return a.folder < b.folder;
        if (a.fields.captured.isValid() != b.fields.captured.isValid())
            return a.fields.captured.isValid();
        if (a.fields.captured != b.fields.captured)
            return a.fields.captured < b.fields.captured;
        return a.captureId < b.captureId;
    });
    return photos;
}

void LibraryRenamer::plan(QList<Photo> &photos, Mode mode, const QString &name, const FileNamePattern &pattern,
                          const QString &author, NameForm form)
{
    // What's in each folder as the renaming reaches each photo: the files on
    // disk, less the names photos before it gave up, plus those they took.
    QHash<QString, QStringList> entries;
    QHash<QString, QStringList> freed;
    QHash<QString, QStringList> takenByBatch;
    QHash<QString, int> sequenceCounters;   // folder + name without its sequence number

    for (Photo &p : photos) {
        p.newStem = p.oldStem;
        if (!p.onDisk)
            continue;
        QString stem;
        if (mode == Mode::NamePart) {
            const QString part = form == NameForm::Typed ? name : speciesName(p.scientificName, p.commonName, form);
            if (part.trimmed().isEmpty())
                continue;
            stem = CaptureRenamer::renamedStem(p.oldStem, p.nameText, part);
        } else {
            collection::NameFields fields = p.fields;
            fields.author = author;
            if (pattern.uses(NameToken::Sequence)) {
                // fields.sequence is still 0 here, so the number is left out.
                const QString key = p.folder + QLatin1Char('\n') + pattern.renderParts(fields).toLower();
                fields.sequence = ++sequenceCounters[key];
            }
            stem = CaptureRenamer::cleanStem(pattern.render(fields));
        }
        if (stem.isEmpty() || stem == p.oldStem)
            continue;

        if (!entries.contains(p.folder))
            entries.insert(p.folder, QDir(p.folder).entryList(QDir::Files | QDir::Hidden));
        const QStringList &files = entries[p.folder];
        const QStringList &gone = freed[p.folder];
        const QStringList &batch = takenByBatch[p.folder];
        const auto taken = [&](const QString &candidate) {
            if (candidate.compare(p.oldStem, Qt::CaseInsensitive) == 0)
                return false;   // its own (a change of case)
            for (const QString &other : batch) {
                if (other.compare(candidate, Qt::CaseInsensitive) == 0)
                    return true;
            }
            for (const QString &entry : files) {
                if (!isNamed(entry, candidate))
                    continue;
                const bool givenUp = std::any_of(gone.cbegin(), gone.cend(),
                                                 [&](const QString &old) { return isNamed(entry, old); });
                if (!givenUp)
                    return true;
            }
            return false;
        };
        if (taken(stem)) {
            // The next free "(n)", as CaptureRenamer counts.
            static const QRegularExpression counter(QStringLiteral("^(.*) \\((\\d+)\\)$"));
            const QRegularExpressionMatch m = counter.match(stem);
            const QString root = m.hasMatch() ? m.captured(1) : stem;
            int n = m.hasMatch() ? m.captured(2).toInt() + 1 : 2;
            while (taken(root + QStringLiteral(" (%1)").arg(n)))
                ++n;
            stem = root + QStringLiteral(" (%1)").arg(n);
        }
        p.newStem = stem;
        freed[p.folder] << p.oldStem;
        takenByBatch[p.folder] << stem;
    }
}

LibraryRenamer::FolderResult LibraryRenamer::renameFolder(const QString &connectionName, int folderId,
                                                         const QString &newName, const QStringList &roots)
{
    FolderResult result;
    QSqlDatabase db = QSqlDatabase::database(connectionName, false);
    QSqlQuery f(db);
    f.prepare(QStringLiteral("SELECT path, name FROM folder WHERE id = ?"));
    f.addBindValue(folderId);
    if (!f.exec() || !f.next()) {
        result.error = QStringLiteral("folder not found");
        return result;
    }
    const QString oldPath = QDir::cleanPath(f.value(0).toString());
    const QString name = cleanName(newName);
    if (name.isEmpty()) {
        result.error = QStringLiteral("no name");
        return result;
    }
    if (name == QFileInfo(oldPath).fileName()) {
        result.newPath = oldPath;
        return result;   // nothing to do
    }
    for (const QString &root : roots) {
        const QString r = QDir::cleanPath(root);
        if (r == oldPath || r.startsWith(oldPath + QLatin1Char('/'))) {
            result.error = QStringLiteral("it's a folder the library watches (under Settings)");
            return result;
        }
    }
    const QString newPath = QFileInfo(oldPath).absoluteDir().filePath(name);
    if (QFileInfo::exists(newPath) && newPath.compare(oldPath, Qt::CaseInsensitive) != 0) {
        result.error = QStringLiteral("there's already a folder called “%1” there").arg(name);
        return result;
    }
    if (!QFileInfo(oldPath).isDir()) {
        result.error = QStringLiteral("the folder isn't on this computer");
        return result;
    }

    const QString prefix = oldPath + QLatin1Char('/');
    const auto moved = [&](const QString &path) { return newPath + path.mid(oldPath.size()); };
    // Everything the catalogue has in it, so all of it can follow.
    struct Row
    {
        qint64 id;
        QString path;
    };
    const auto rowsUnder = [&](const QString &table, QList<Row> &rows) {
        QSqlQuery q(db);
        q.prepare(QStringLiteral("SELECT id, path FROM %1 WHERE path = ? OR substr(path, 1, ?) = ?").arg(table));
        q.addBindValue(oldPath);
        q.addBindValue(int(prefix.size()));
        q.addBindValue(prefix);
        if (!q.exec())
            return false;
        while (q.next())
            rows.append({q.value(0).toLongLong(), q.value(1).toString()});
        return true;
    };
    QList<Row> folders, files;
    if (!rowsUnder(QStringLiteral("folder"), folders) || !rowsUnder(QStringLiteral("rendition"), files)) {
        result.error = QStringLiteral("couldn't read the catalogue");
        return result;
    }
    const int online = int(std::count_if(files.cbegin(), files.cend(),
                                         [](const Row &r) { return !QFileInfo::exists(r.path); }));
    if (online > 0) {
        result.error = QStringLiteral("%1 of its files aren't on this computer (kept online only)").arg(online);
        return result;
    }

    if (!QDir().rename(oldPath, newPath)) {
        result.error = QStringLiteral("couldn't rename the folder");
        return result;
    }
    bool ok = db.transaction();
    for (const Row &r : std::as_const(folders)) {
        if (!ok)
            break;
        QSqlQuery u(db);
        if (r.id == folderId) {
            u.prepare(QStringLiteral("UPDATE folder SET path = ?, name = ? WHERE id = ?"));
            u.addBindValue(newPath);
            u.addBindValue(name);
        } else {
            u.prepare(QStringLiteral("UPDATE folder SET path = ? WHERE id = ?"));
            u.addBindValue(moved(r.path));
        }
        u.addBindValue(r.id);
        ok = u.exec();
        if (!ok)
            result.error = u.lastError().text();
    }
    for (const Row &r : std::as_const(files)) {
        if (!ok)
            break;
        QSqlQuery u(db);
        u.prepare(QStringLiteral("UPDATE rendition SET path = ? WHERE id = ?"));
        u.addBindValue(moved(r.path));
        u.addBindValue(r.id);
        ok = u.exec();
        if (!ok)
            result.error = u.lastError().text();
    }
    if (!ok || !db.commit()) {
        db.rollback();
        QDir().rename(newPath, oldPath);
        if (result.error.isEmpty())
            result.error = QStringLiteral("couldn't update the catalogue");
        return result;
    }
    result.renamed = true;
    result.newPath = newPath;
    return result;
}

} // namespace pl::scan
