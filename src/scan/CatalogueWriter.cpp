#include "scan/CatalogueWriter.h"

#include "db/Database.h"
#include "raw/RawPreview.h"
#include "scan/UnsortedFolders.h"
#include "scan/Exif.h"
#include "scan/FilenameParser.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include <optional>

namespace pl::scan {
namespace {

// A shot rated 5 stars: in a JPEG's own XMP, or in an XMP sidecar beside the
// RAW ("DSC_1234.xmp" as Lightroom names it, "DSC_1234.NEF.xmp" as darktable does).
bool isFiveStar(const DiscoveredCapture &cap)
{
    QStringList sources;
    sources << QDir(cap.dirPath).filePath(cap.baseName + QStringLiteral(".xmp"));
    for (const DiscoveredFile &f : cap.files) {
        if (!f.isRaw)
            sources << f.path;
        sources << f.path + QStringLiteral(".xmp");
    }
    for (const QString &path : std::as_const(sources)) {
        if (QFileInfo::exists(path) && readXmpRating(path).value_or(0) >= 5)
            return true;
    }
    return false;
}

// A location in an XMP sidecar beside the shot (Set Location on Map, or
// another program): for a RAW, which PhotoLife doesn't write to.
std::optional<ExifData> sidecarGps(const DiscoveredCapture &cap)
{
    QStringList sources;
    sources << QDir(cap.dirPath).filePath(cap.baseName + QStringLiteral(".xmp"));
    for (const DiscoveredFile &f : cap.files)
        sources << f.path + QStringLiteral(".xmp");
    for (const QString &path : std::as_const(sources)) {
        if (!QFileInfo::exists(path))
            continue;
        const ExifData xmp = readXmpGps(path);
        if (xmp.hasGps())
            return xmp;
    }
    return std::nullopt;
}

QString isoNow()
{
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
}

QString normalise(const QString &path)
{
    return QDir::cleanPath(path);
}

// Everything read from a file's bytes in one pass: content hash, pixel size,
// and (for JPEGs) EXIF.
struct RenditionProbe
{
    QString contentHash;
    int width = 0;
    int height = 0;
    ExifData exif;
};

RenditionProbe probeFile(const DiscoveredFile &file)
{
    RenditionProbe out;

    QFile f(file.path);
    if (f.open(QIODevice::ReadOnly)) {
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (hash.addData(&f))
            out.contentHash = QString::fromLatin1(hash.result().toHex());
    }

    if (!file.isRaw) {
        QImageReader reader(file.path);
        const QSize size = reader.size();
        if (size.isValid()) {
            out.width = size.width();
            out.height = size.height();
        }
        if (file.ext == QLatin1String("jpg") || file.ext == QLatin1String("jpeg")
            || file.ext == QLatin1String("jpe")) {
            out.exif = readJpegExif(file.path);
        }
    } else {
        // Cameras typically stamp the embedded preview with the same
        // date/camera EXIF as the original capture, so re-use the JPEG
        // parser on it for those fields. Not for GPS, though: LibRaw's
        // reconstructed copy of the embedded preview's EXIF can corrupt
        // small inline fields (GPSLatitudeRef/GPSLongitudeRef observed
        // garbled), silently defaulting the hemisphere sign to positive —
        // use LibRaw's own metadata parser for that instead.
        const QByteArray jpegBytes = raw::extractEmbeddedJpegBytes(file.path);
        if (!jpegBytes.isEmpty()) {
            QBuffer buf;
            buf.setData(jpegBytes);
            buf.open(QIODevice::ReadOnly);
            out.exif = readJpegExif(buf);
        }
        if (const raw::RawGps gps = raw::extractGps(file.path); gps.latitude && gps.longitude) {
            out.exif.latitude = gps.latitude;
            out.exif.longitude = gps.longitude;
        }
    }

    return out;
}

QString renditionKind(const DiscoveredFile &file)
{
    return file.isRaw ? QStringLiteral("raw") : QStringLiteral("jpeg");
}

} // namespace

CatalogueWriter::CatalogueWriter(QString connectionName)
    : m_connectionName(std::move(connectionName))
{
}

QString CatalogueWriter::rootForDir(const QString &dirPath, const QStringList &roots) const
{
    QString best;
    const QString dir = normalise(dirPath);
    for (const QString &raw : roots) {
        const QString root = normalise(raw);
        if (dir == root || dir.startsWith(root + QLatin1Char('/'))) {
            if (root.length() > best.length())
                best = root;
        }
    }
    return best;
}

int CatalogueWriter::folderIdFor(const QString &dirPath, const QString &rootPath)
{
    const QString path = normalise(dirPath);
    if (const auto it = m_folderIds.constFind(path); it != m_folderIds.constEnd())
        return *it;

    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);

    int parentId = -1;
    int depth = 0;
    if (!rootPath.isEmpty() && path != rootPath && path.startsWith(rootPath + QLatin1Char('/'))) {
        const QString parentPath = normalise(QFileInfo(path).path());
        parentId = folderIdFor(parentPath, rootPath);
        QSqlQuery pd(db);
        pd.prepare(QStringLiteral("SELECT depth FROM folder WHERE id = ?"));
        pd.addBindValue(parentId);
        if (pd.exec() && pd.next())
            depth = pd.value(0).toInt() + 1;
    }

    QSqlQuery find(db);
    find.prepare(QStringLiteral("SELECT id FROM folder WHERE path = ?"));
    find.addBindValue(path);
    if (find.exec() && find.next()) {
        const int id = find.value(0).toInt();
        m_folderIds.insert(path, id);
        return id;
    }

    QSqlQuery ins(db);
    // RETURNING id (SQLite 3.35+, Postgres) instead of lastInsertId(): Qt's
    // QPSQL driver has no reliable lastInsertId() on modern Postgres (row
    // OIDs are gone), so this is the one code path that works on both.
    ins.prepare(QStringLiteral(
        "INSERT INTO folder (path, parent_id, name, depth) VALUES (?, ?, ?, ?) RETURNING id"));
    ins.addBindValue(path);
    ins.addBindValue(parentId < 0 ? QVariant() : QVariant(parentId));
    ins.addBindValue(QFileInfo(path).fileName());
    ins.addBindValue(depth);
    ins.exec();

    const int id = ins.next() ? ins.value(0).toInt() : -1;
    m_folderIds.insert(path, id);
    ++m_stats.foldersSeen;
    return id;
}

ScanSummary CatalogueWriter::sync(const QList<DiscoveredCapture> &captures,
                                  const QStringList &roots)
{
    ScanSummary summary;
    m_folderIds.clear();
    m_stats = {};

    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    if (!db.isOpen()) {
        summary.error = QStringLiteral("catalogue connection is not open");
        return summary;
    }
    if (!db.transaction()) {
        summary.error = QStringLiteral("cannot begin transaction: %1")
                            .arg(db.lastError().text());
        return summary;
    }

    auto bail = [&](const QString &what) {
        db.rollback();
        summary.error = what;
        return summary;
    };

    // On the shared Postgres backend, a full library scan can touch tens of
    // thousands of rows; holding one giant transaction for the whole run
    // would lock folder/capture/rendition against other collaborators for
    // its entire duration. SQLite keeps the original one-transaction-per-run
    // behaviour, since it has no other writer to block.
    const bool chunkCommits = Database::backendFor(m_connectionName)
                               == CatalogueDescriptor::Backend::Postgres;
    constexpr int kCommitEvery = 200;

    const QString now = isoNow();

    // Unsorted folders: one photo per run is catalogued; the rest are only
    // remembered (unsorted_photo). Files remembered already and unchanged
    // are skipped here, with no further work; a new run sends just its
    // representative on to be catalogued below.
    const UnsortedPlan unsorted = planUnsorted(captures, roots);
    if (!unsorted.error.isEmpty())
        return bail(unsorted.error);
    summary.renditionsUnchanged += unsorted.skippedFiles;
    const QList<DiscoveredCapture> &work = unsorted.active ? unsorted.work : captures;

    for (const DiscoveredCapture &cap : work) {
        if (cancelled()) {
            summary.cancelled = true;
            break;
        }

        const QString root = rootForDir(cap.dirPath, roots);
        const int folderId = folderIdFor(cap.dirPath, root);
        if (folderId <= 0)
            return bail(QStringLiteral("failed to record folder %1").arg(cap.dirPath));

        const ParsedFilename parsed = parseStem(cap.baseName);
        int renamedRenditionId = -1;
        QString renamedPath;
        std::optional<RenditionProbe> renamedProbe;

        // Upsert the capture row (date filled in after the renditions are probed).
        QSqlQuery findCap(db);
        findCap.prepare(QStringLiteral(
            "SELECT id, captured_on, time_checked FROM capture WHERE folder_id = ? AND base_name = ?"));
        findCap.addBindValue(folderId);
        findCap.addBindValue(cap.baseName);
        if (!findCap.exec())
            return bail(findCap.lastError().text());

        int captureId = -1;
        bool captureExisted = findCap.next();
        // What it has already (a renamed photo's files are read again anyway).
        const QString storedOn = captureExisted ? findCap.value(1).toString() : QString();
        const bool timeChecked = captureExisted && findCap.value(2).toInt() != 0;
        if (!captureExisted) {
            // A rename changes both the rendition path and capture stem. Reuse
            // the old capture when its file is gone but its content hash is
            // found at the new path, preserving matches and thumbnails.
            for (const DiscoveredFile &file : cap.files) {
                const RenditionProbe probe = probeFile(file);
                if (probe.contentHash.isEmpty())
                    continue;

                QSqlQuery findRenamed(db);
                findRenamed.prepare(QStringLiteral(
                    "SELECT r.id, r.capture_id, r.path FROM rendition r "
                    "JOIN capture c ON c.id = r.capture_id "
                    "WHERE r.content_hash = ? AND r.path <> ? "
                    "ORDER BY (c.folder_id = ?) DESC, r.id"));
                findRenamed.addBindValue(probe.contentHash);
                findRenamed.addBindValue(file.path);
                findRenamed.addBindValue(folderId);
                if (!findRenamed.exec())
                    return bail(findRenamed.lastError().text());

                while (findRenamed.next()) {
                    const QString oldPath = findRenamed.value(2).toString();
                    if (QFileInfo::exists(oldPath))
                        continue;
                    renamedRenditionId = findRenamed.value(0).toInt();
                    captureId = findRenamed.value(1).toInt();
                    renamedPath = file.path;
                    renamedProbe = probe;
                    captureExisted = true;
                    break;
                }
                if (captureExisted)
                    break;
            }
        }

        if (captureExisted) {
            if (captureId < 0)
                captureId = findCap.value(0).toInt();
            QSqlQuery upd(db);
            upd.prepare(QStringLiteral(
                "UPDATE capture SET folder_id = ?, base_name = ?, name_text = ?, "
                "locality_text = ?, organ_tags = ?, last_seen = ? WHERE id = ?"));
            upd.addBindValue(folderId);
            upd.addBindValue(cap.baseName);
            upd.addBindValue(parsed.name);
            upd.addBindValue(parsed.locality.isEmpty() ? QVariant() : parsed.locality);
            upd.addBindValue(parsed.organTags.isEmpty() ? QVariant()
                                                        : parsed.organTags.join(QLatin1Char(',')));
            upd.addBindValue(now);
            upd.addBindValue(captureId);
            if (!upd.exec())
                return bail(upd.lastError().text());
            ++summary.capturesUpdated;
        } else {
            QSqlQuery ins(db);
            // RETURNING id instead of lastInsertId() -- see the folder
            // insert above for why.
            ins.prepare(QStringLiteral(
                "INSERT INTO capture (folder_id, base_name, name_text, locality_text, "
                "organ_tags, date_source, first_seen, last_seen) "
                "VALUES (?, ?, ?, ?, ?, 'none', ?, ?) RETURNING id"));
            ins.addBindValue(folderId);
            ins.addBindValue(cap.baseName);
            ins.addBindValue(parsed.name);
            ins.addBindValue(parsed.locality.isEmpty() ? QVariant() : parsed.locality);
            ins.addBindValue(parsed.organTags.isEmpty() ? QVariant()
                                                        : parsed.organTags.join(QLatin1Char(',')));
            ins.addBindValue(now);
            ins.addBindValue(now);
            if (!ins.exec() || !ins.next())
                return bail(ins.lastError().text());
            captureId = ins.value(0).toInt();
            ++summary.capturesAdded;
            // Rated 5 stars before it came in (Organise's Best Shot, or
            // another photo program): a best shot. Only when first added, so
            // unstarring it here later sticks.
            if (isFiveStar(cap)) {
                QSqlQuery best(db);
                best.prepare(QStringLiteral("INSERT INTO best_shot (capture_id) VALUES (?) "
                                            "ON CONFLICT(capture_id) DO NOTHING"));
                best.addBindValue(captureId);
                if (!best.exec())
                    return bail(best.lastError().text());
            }
        }

        QDateTime earliestExif;
        std::optional<double> lat, lon;
        for (const DiscoveredFile &file : cap.files) {
            if (cancelled()) {
                summary.cancelled = true;
                break;
            }

            if (file.path == renamedPath) {
                const RenditionProbe &probe = *renamedProbe;
                QSqlQuery upd(db);
                upd.prepare(QStringLiteral(
                    "UPDATE rendition SET capture_id = ?, path = ?, kind = ?, ext = ?, "
                    "file_size = ?, mtime = ?, width = ?, height = ?, scanned_at = ?, "
                    "geo_checked = 1 WHERE id = ?"));
                upd.addBindValue(captureId);
                upd.addBindValue(file.path);
                upd.addBindValue(renditionKind(file));
                upd.addBindValue(file.ext);
                upd.addBindValue(file.size);
                upd.addBindValue(file.mtime);
                upd.addBindValue(probe.width > 0 ? QVariant(probe.width) : QVariant());
                upd.addBindValue(probe.height > 0 ? QVariant(probe.height) : QVariant());
                upd.addBindValue(now);
                upd.addBindValue(renamedRenditionId);
                if (!upd.exec())
                    return bail(upd.lastError().text());
                ++summary.renditionsUpdated;
                if (probe.exif.hasDate())
                    earliestExif = probe.exif.dateTimeOriginal;
                if (probe.exif.hasGps()) {
                    lat = probe.exif.latitude;
                    lon = probe.exif.longitude;
                }
                continue;
            }

            QSqlQuery findRend(db);
            findRend.prepare(QStringLiteral(
                "SELECT id, file_size, mtime, geo_checked FROM rendition WHERE path = ?"));
            findRend.addBindValue(file.path);
            if (!findRend.exec())
                return bail(findRend.lastError().text());

            const bool rendExisted = findRend.next();
            const int rendId = rendExisted ? findRend.value(0).toInt() : -1;
            const bool sameFile = rendExisted
                                  && findRend.value(1).toLongLong() == file.size
                                  && findRend.value(2).toLongLong() == file.mtime;
            const bool geoChecked = rendExisted && findRend.value(3).toInt() != 0;
            // A file catalogued before GPS support existed is otherwise
            // "unchanged" forever and would never get a chance to backfill
            // its coordinates — so an unchecked rendition still gets probed
            // once even if its file itself hasn't changed.
            const bool unchanged = sameFile && geoChecked;

            if (unchanged) {
                ++summary.renditionsUnchanged;
                continue;
            }

            const RenditionProbe probe = probeFile(file);
            ++summary.filesHashed;
            if (probe.exif.hasDate()
                && (!earliestExif.isValid() || probe.exif.dateTimeOriginal < earliestExif)) {
                earliestExif = probe.exif.dateTimeOriginal;
            }
            if (!lat && probe.exif.hasGps()) {
                lat = probe.exif.latitude;
                lon = probe.exif.longitude;
            }

            if (rendExisted) {
                QSqlQuery upd(db);
                upd.prepare(QStringLiteral(
                    "UPDATE rendition SET capture_id = ?, kind = ?, ext = ?, content_hash = ?, "
                    "file_size = ?, mtime = ?, width = ?, height = ?, scanned_at = ?, "
                    "geo_checked = 1 WHERE id = ?"));
                upd.addBindValue(captureId);
                upd.addBindValue(renditionKind(file));
                upd.addBindValue(file.ext);
                upd.addBindValue(probe.contentHash.isEmpty() ? QVariant() : probe.contentHash);
                upd.addBindValue(file.size);
                upd.addBindValue(file.mtime);
                upd.addBindValue(probe.width > 0 ? QVariant(probe.width) : QVariant());
                upd.addBindValue(probe.height > 0 ? QVariant(probe.height) : QVariant());
                upd.addBindValue(now);
                upd.addBindValue(rendId);
                if (!upd.exec())
                    return bail(upd.lastError().text());
                if (sameFile)
                    ++summary.renditionsUnchanged;   // only the geo backfill ran
                else
                    ++summary.renditionsUpdated;
            } else {
                QSqlQuery ins(db);
                ins.prepare(QStringLiteral(
                    "INSERT INTO rendition (capture_id, path, kind, ext, content_hash, "
                    "file_size, mtime, width, height, scanned_at, geo_checked) "
                    "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 1)"));
                ins.addBindValue(captureId);
                ins.addBindValue(file.path);
                ins.addBindValue(renditionKind(file));
                ins.addBindValue(file.ext);
                ins.addBindValue(probe.contentHash.isEmpty() ? QVariant() : probe.contentHash);
                ins.addBindValue(file.size);
                ins.addBindValue(file.mtime);
                ins.addBindValue(probe.width > 0 ? QVariant(probe.width) : QVariant());
                ins.addBindValue(probe.height > 0 ? QVariant(probe.height) : QVariant());
                ins.addBindValue(now);
                if (!ins.exec())
                    return bail(ins.lastError().text());
                ++summary.renditionsAdded;
            }
        }

        // No time from the files read just now, and none stored: look once
        // for the camera's clock time -- a RAW's own metadata (its embedded
        // preview's EXIF often has no date), or a JPEG's EXIF.
        QDateTime fileTime = earliestExif;
        bool checkedNow = false;
        if (!fileTime.isValid() && !timeChecked && storedOn.size() <= 10 && !summary.cancelled) {
            for (const DiscoveredFile &file : cap.files) {
                if (file.isRaw)
                    fileTime = raw::readInfo(file.path).taken;
                else if (file.ext == QLatin1String("jpg") || file.ext == QLatin1String("jpeg")
                         || file.ext == QLatin1String("jpe"))
                    fileTime = readJpegExif(file.path).dateTimeOriginal;
                if (fileTime.isValid())
                    break;
            }
            checkedNow = true;
        }
        // A time already stored, when nothing newer was read.
        const QDateTime storedTime = storedOn.size() > 10 ? QDateTime::fromString(storedOn, Qt::ISODate) : QDateTime();
        const QDateTime knownTime = fileTime.isValid() ? fileTime : storedTime;

        // Reconcile the capture date: a filename date always wins (film-era
        // scans have a scan-date EXIF but a correct filename); the camera's
        // time is kept when it's on that day, and is the date otherwise.
        QString capturedOn;
        QString dateSource = QStringLiteral("none");
        if (parsed.capturedOn.isValid()) {
            if (knownTime.isValid() && knownTime.date() == parsed.capturedOn) {
                capturedOn = knownTime.toString(Qt::ISODate);
                dateSource = QStringLiteral("exif");
            } else {
                capturedOn = parsed.capturedOn.toString(Qt::ISODate);
                dateSource = QStringLiteral("filename");
            }
        } else if (fileTime.isValid()) {
            capturedOn = fileTime.toString(Qt::ISODate);
            dateSource = QStringLiteral("exif");
        }

        if (!capturedOn.isEmpty() || !captureExisted || checkedNow) {
            QSqlQuery setDate(db);
            if (!capturedOn.isEmpty() || !captureExisted) {
                setDate.prepare(QStringLiteral(
                    "UPDATE capture SET captured_on = ?, date_source = ?, time_checked = ? WHERE id = ?"));
                setDate.addBindValue(capturedOn.isEmpty() ? QVariant() : capturedOn);
                setDate.addBindValue(dateSource);
            } else {
                setDate.prepare(QStringLiteral("UPDATE capture SET time_checked = ? WHERE id = ?"));
            }
            setDate.addBindValue(timeChecked || checkedNow || earliestExif.isValid() ? 1 : 0);
            setDate.addBindValue(captureId);
            if (!setDate.exec())
                return bail(setDate.lastError().text());
        }

        // A new photo without GPS in its files may have it in a sidecar.
        if (!captureExisted && !(lat && lon)) {
            if (const std::optional<ExifData> xmp = sidecarGps(cap)) {
                lat = xmp->latitude;
                lon = xmp->longitude;
            }
        }

        // Only write GPS when found: unchanged renditions are skipped above
        // (before probeFile() runs), so an incremental rescan where nothing
        // changed must not blank out coordinates already stored.
        if (lat && lon) {
            QSqlQuery setGeo(db);
            setGeo.prepare(QStringLiteral(
                "UPDATE capture SET latitude = ?, longitude = ? WHERE id = ?"));
            setGeo.addBindValue(*lat);
            setGeo.addBindValue(*lon);
            setGeo.addBindValue(captureId);
            if (!setGeo.exec())
                return bail(setGeo.lastError().text());
        }

        ++m_stats.capturesSeen;
        if (chunkCommits && m_stats.capturesSeen % kCommitEvery == 0) {
            if (!db.commit())
                return bail(QStringLiteral("commit failed: %1").arg(db.lastError().text()));
            if (!db.transaction()) {
                summary.error = QStringLiteral("cannot begin transaction: %1")
                                    .arg(db.lastError().text());
                return summary;
            }
        }
        if (m_progress && (m_stats.capturesSeen % kCommitEvery) == 0)
            m_progress(m_stats);
    }

    if (unsorted.active && !summary.cancelled) {
        if (const QString error = rememberUnsorted(unsorted); !error.isEmpty())
            return bail(error);
    }

    if (!db.commit())
        return bail(QStringLiteral("commit failed: %1").arg(db.lastError().text()));

    summary.foldersUpserted = m_folderIds.size();
    return summary;
}

CatalogueWriter::UnsortedPlan CatalogueWriter::planUnsorted(const QList<DiscoveredCapture> &captures,
                                                            const QStringList &roots)
{
    UnsortedPlan plan;
    plan.folders = Unsorted::folders(m_connectionName);
    if (plan.folders.isEmpty())
        return plan;
    plan.active = true;
    plan.roots = roots;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);

    // What's remembered, and which photos there are catalogued.
    struct Known
    {
        qint64 size = 0;
        qint64 mtime = 0;
        int captureId = 0;
    };
    QHash<QString, Known> known;
    {
        QSqlQuery q(db);
        q.setForwardOnly(true);
        if (!q.exec(QStringLiteral("SELECT path, file_size, mtime, capture_id FROM unsorted_photo"))) {
            plan.error = q.lastError().text();
            return plan;
        }
        while (q.next())
            known.insert(q.value(0).toString(), {q.value(1).toLongLong(), q.value(2).toLongLong(), q.value(3).toInt()});
    }
    QSet<QString> catalogued;   // "dir/base" of captures in Unsorted folders
    QHash<int, QString> repPaths;   // representative capture -> one of its files
    {
        QSqlQuery q(db);
        q.setForwardOnly(true);
        if (!q.exec(QStringLiteral("SELECT f.path, c.base_name, c.id, "
                                   "  (SELECT r.path FROM rendition r WHERE r.capture_id = c.id ORDER BY r.id LIMIT 1) "
                                   "FROM capture c JOIN folder f ON f.id = c.folder_id"))) {
            plan.error = q.lastError().text();
            return plan;
        }
        while (q.next()) {
            const QString dir = q.value(0).toString();
            if (!Unsorted::contains(plan.folders, dir))
                continue;
            catalogued.insert(dir + QLatin1Char('/') + q.value(1).toString());
            repPaths.insert(q.value(2).toInt(), q.value(3).toString());
        }
    }

    QList<DiscoveredCapture> fresh;
    QHash<int, QList<const DiscoveredCapture *>> membersByRep;   // remembered and still there
    for (const DiscoveredCapture &cap : captures) {
        for (const DiscoveredFile &f : cap.files)
            plan.seen.insert(f.path);
        const QString dir = normalise(cap.dirPath);
        if (!Unsorted::contains(plan.folders, dir) || catalogued.contains(dir + QLatin1Char('/') + cap.baseName)) {
            plan.work << cap;
            continue;
        }
        bool allKnown = !cap.files.isEmpty();
        int rep = 0;
        for (const DiscoveredFile &f : cap.files) {
            const auto it = known.constFind(f.path);
            if (it == known.constEnd() || it->size != f.size || it->mtime != f.mtime) {
                allKnown = false;
                break;
            }
            rep = it->captureId;
        }
        if (allKnown) {
            plan.skippedFiles += int(cap.files.size());
            membersByRep[rep] << &cap;
            continue;
        }
        fresh << cap;
    }

    // A representative whose files are gone (culled) while others of its run
    // remain: another of them takes its place, so the species stays.
    for (auto it = membersByRep.cbegin(); it != membersByRep.cend(); ++it) {
        const QString repFile = repPaths.value(it.key());
        if (it.key() > 0 && !repFile.isEmpty() && plan.seen.contains(repFile))
            continue;
        if (it.key() > 0 && (repFile.isEmpty() || rootForDir(QFileInfo(repFile).path(), roots).isEmpty()))
            continue;   // not in what was scanned: can't tell it's gone
        const QList<const DiscoveredCapture *> &members = it.value();
        const DiscoveredCapture *promoted = members.at(members.size() / 2);
        plan.work << *promoted;
        UnsortedPlan::Promotion promotion;
        promotion.oldCaptureId = it.key();
        promotion.dirPath = normalise(promoted->dirPath);
        promotion.baseName = promoted->baseName;
        for (const DiscoveredFile &f : promoted->files)
            promotion.files << f.path;
        plan.promotions << promotion;
    }

    // New photos: into runs, by when they were taken.
    QList<RunItem> items;
    for (const DiscoveredCapture &cap : std::as_const(fresh)) {
        RunItem item;
        item.group = Unsorted::groupKey(cap.dirPath, cap.baseName);
        for (const DiscoveredFile &f : cap.files)
            if (const QDateTime t = Unsorted::fileTime(f); t.isValid()) {
                item.taken = t;
                break;
            }
        item.day = item.taken.isValid() ? item.taken.date() : parseStem(cap.baseName).capturedOn;
        items << item;
    }
    for (const QList<int> &run : groupRuns(items)) {
        // A representative already catalogued from this run (shots added to it)?
        const DiscoveredCapture &first = fresh.at(run.first());
        int existing = 0;
        {
            QSqlQuery q(db);
            q.prepare(QStringLiteral("SELECT c.id, c.base_name, c.captured_on FROM capture c JOIN folder f ON f.id = c.folder_id "
                                     "WHERE f.path = ?"));
            q.addBindValue(normalise(first.dirPath));
            if (q.exec()) {
                const QString group = items.at(run.first()).group;
                while (q.next() && existing == 0) {
                    if (Unsorted::groupKey(first.dirPath, q.value(1).toString()) != group)
                        continue;
                    const QString on = q.value(2).toString();
                    const QDateTime t = on.size() > 10 ? QDateTime::fromString(on.left(19), Qt::ISODate) : QDateTime();
                    for (int i : run) {
                        const RunItem &it = items.at(i);
                        const bool near = it.taken.isValid() && t.isValid()
                                              ? std::abs(t.secsTo(it.taken)) <= kRunGapSeconds
                                              : !it.taken.isValid() && !t.isValid()
                                                    && QDate::fromString(on.left(10), Qt::ISODate) == it.day;
                        if (near) {
                            existing = q.value(0).toInt();
                            break;
                        }
                    }
                }
            }
        }
        const int repIndex = existing > 0 ? -1 : run.at(run.size() / 2);
        if (repIndex >= 0)
            plan.work << fresh.at(repIndex);
        for (int i : run) {
            if (i == repIndex)
                continue;
            const DiscoveredCapture &cap = fresh.at(i);
            for (const DiscoveredFile &f : cap.files) {
                UnsortedPlan::Member m;
                m.path = f.path;
                m.dirPath = normalise(cap.dirPath);
                m.baseName = cap.baseName;
                m.size = f.size;
                m.mtime = f.mtime;
                m.taken = items.at(i).taken.isValid() ? items.at(i).taken.toString(Qt::ISODate)
                          : items.at(i).day.isValid() ? items.at(i).day.toString(Qt::ISODate)
                                                      : QString();
                m.repCaptureId = existing;
                if (repIndex >= 0) {
                    m.repDirPath = normalise(fresh.at(repIndex).dirPath);
                    m.repBaseName = fresh.at(repIndex).baseName;
                }
                plan.members << m;
            }
        }
    }
    return plan;
}

QString CatalogueWriter::rememberUnsorted(const UnsortedPlan &plan)
{
    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    auto captureIdOf = [&](const QString &dirPath, const QString &baseName) {
        QSqlQuery q(db);
        q.prepare(QStringLiteral("SELECT c.id FROM capture c JOIN folder f ON f.id = c.folder_id "
                                 "WHERE f.path = ? AND c.base_name = ?"));
        q.addBindValue(dirPath);
        q.addBindValue(baseName);
        return q.exec() && q.next() ? q.value(0).toInt() : 0;
    };
    // New runs' other shots, remembered with their representative.
    for (const UnsortedPlan::Member &m : plan.members) {
        const int rep = m.repCaptureId > 0 ? m.repCaptureId : captureIdOf(m.repDirPath, m.repBaseName);
        QSqlQuery del(db);
        del.prepare(QStringLiteral("DELETE FROM unsorted_photo WHERE path = ?"));
        del.addBindValue(m.path);
        del.exec();
        QSqlQuery ins(db);
        ins.prepare(QStringLiteral(
            "INSERT INTO unsorted_photo (path, folder_id, base_name, file_size, mtime, taken, capture_id) "
            "VALUES (?, (SELECT id FROM folder WHERE path = ?), ?, ?, ?, ?, ?)"));
        ins.addBindValue(m.path);
        ins.addBindValue(m.dirPath);
        ins.addBindValue(m.baseName);
        ins.addBindValue(m.size);
        ins.addBindValue(m.mtime);
        ins.addBindValue(m.taken.isEmpty() ? QVariant() : m.taken);
        ins.addBindValue(rep > 0 ? QVariant(rep) : QVariant());
        if (!ins.exec())
            return ins.lastError().text();
    }
    // A promoted shot is catalogued now: the rest of its run follow it.
    for (const UnsortedPlan::Promotion &p : plan.promotions) {
        const int rep = captureIdOf(p.dirPath, p.baseName);
        if (rep <= 0)
            continue;
        for (const QString &path : p.files) {
            QSqlQuery del(db);
            del.prepare(QStringLiteral("DELETE FROM unsorted_photo WHERE path = ?"));
            del.addBindValue(path);
            del.exec();
        }
        QSqlQuery move(db);
        move.prepare(p.oldCaptureId > 0
                         ? QStringLiteral("UPDATE unsorted_photo SET capture_id = ? WHERE capture_id = ?")
                         : QStringLiteral("UPDATE unsorted_photo SET capture_id = ? WHERE capture_id IS NULL "
                                          "AND folder_id = (SELECT id FROM folder WHERE path = ?)"));
        move.addBindValue(rep);
        if (p.oldCaptureId > 0)
            move.addBindValue(p.oldCaptureId);
        else
            move.addBindValue(p.dirPath);
        if (!move.exec())
            return move.lastError().text();
    }
    // Remembered files that are gone (culled), within the folders scanned.
    {
        QSqlQuery q(db);
        q.setForwardOnly(true);
        QStringList gone;
        if (q.exec(QStringLiteral("SELECT path FROM unsorted_photo")))
            while (q.next()) {
                const QString path = q.value(0).toString();
                if (!plan.seen.contains(path) && !rootForDir(QFileInfo(path).path(), plan.roots).isEmpty())
                    gone << path;
            }
        for (const QString &path : std::as_const(gone)) {
            QSqlQuery del(db);
            del.prepare(QStringLiteral("DELETE FROM unsorted_photo WHERE path = ?"));
            del.addBindValue(path);
            if (!del.exec())
                return del.lastError().text();
        }
    }
    return {};
}

} // namespace pl::scan
