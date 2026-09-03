#include "scan/CatalogueWriter.h"

#include "scan/Exif.h"
#include "scan/FilenameParser.h"

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

namespace pl::scan {
namespace {

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
    ins.prepare(QStringLiteral(
        "INSERT INTO folder (path, parent_id, name, depth) VALUES (?, ?, ?, ?)"));
    ins.addBindValue(path);
    ins.addBindValue(parentId < 0 ? QVariant() : QVariant(parentId));
    ins.addBindValue(QFileInfo(path).fileName());
    ins.addBindValue(depth);
    ins.exec();

    const int id = ins.lastInsertId().toInt();
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

    const QString now = isoNow();

    for (const DiscoveredCapture &cap : captures) {
        if (cancelled()) {
            summary.cancelled = true;
            break;
        }

        const QString root = rootForDir(cap.dirPath, roots);
        const int folderId = folderIdFor(cap.dirPath, root);
        if (folderId <= 0)
            return bail(QStringLiteral("failed to record folder %1").arg(cap.dirPath));

        const ParsedFilename parsed = parseStem(cap.baseName);

        // Upsert the capture row (date filled in after the renditions are probed).
        QSqlQuery findCap(db);
        findCap.prepare(QStringLiteral(
            "SELECT id FROM capture WHERE folder_id = ? AND base_name = ?"));
        findCap.addBindValue(folderId);
        findCap.addBindValue(cap.baseName);
        if (!findCap.exec())
            return bail(findCap.lastError().text());

        int captureId = -1;
        const bool captureExisted = findCap.next();
        if (captureExisted) {
            captureId = findCap.value(0).toInt();
            QSqlQuery upd(db);
            upd.prepare(QStringLiteral(
                "UPDATE capture SET name_text = ?, locality_text = ?, organ_tags = ?, "
                "last_seen = ? WHERE id = ?"));
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
            ins.prepare(QStringLiteral(
                "INSERT INTO capture (folder_id, base_name, name_text, locality_text, "
                "organ_tags, date_source, first_seen, last_seen) "
                "VALUES (?, ?, ?, ?, ?, 'none', ?, ?)"));
            ins.addBindValue(folderId);
            ins.addBindValue(cap.baseName);
            ins.addBindValue(parsed.name);
            ins.addBindValue(parsed.locality.isEmpty() ? QVariant() : parsed.locality);
            ins.addBindValue(parsed.organTags.isEmpty() ? QVariant()
                                                        : parsed.organTags.join(QLatin1Char(',')));
            ins.addBindValue(now);
            ins.addBindValue(now);
            if (!ins.exec())
                return bail(ins.lastError().text());
            captureId = ins.lastInsertId().toInt();
            ++summary.capturesAdded;
        }

        QDateTime earliestExif;
        for (const DiscoveredFile &file : cap.files) {
            if (cancelled()) {
                summary.cancelled = true;
                break;
            }

            QSqlQuery findRend(db);
            findRend.prepare(QStringLiteral(
                "SELECT id, file_size, mtime FROM rendition WHERE path = ?"));
            findRend.addBindValue(file.path);
            if (!findRend.exec())
                return bail(findRend.lastError().text());

            const bool rendExisted = findRend.next();
            const int rendId = rendExisted ? findRend.value(0).toInt() : -1;
            const bool unchanged = rendExisted
                                   && findRend.value(1).toLongLong() == file.size
                                   && findRend.value(2).toLongLong() == file.mtime;

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

            if (rendExisted) {
                QSqlQuery upd(db);
                upd.prepare(QStringLiteral(
                    "UPDATE rendition SET capture_id = ?, kind = ?, ext = ?, content_hash = ?, "
                    "file_size = ?, mtime = ?, width = ?, height = ?, scanned_at = ? WHERE id = ?"));
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
                ++summary.renditionsUpdated;
            } else {
                QSqlQuery ins(db);
                ins.prepare(QStringLiteral(
                    "INSERT INTO rendition (capture_id, path, kind, ext, content_hash, "
                    "file_size, mtime, width, height, scanned_at) "
                    "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
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

        // Reconcile the capture date: a filename date always wins; EXIF is the
        // fallback (film-era scans have a scan-date EXIF but a correct filename).
        QString capturedOn;
        QString dateSource = QStringLiteral("none");
        if (parsed.capturedOn.isValid()) {
            capturedOn = parsed.capturedOn.toString(Qt::ISODate);
            dateSource = QStringLiteral("filename");
        } else if (earliestExif.isValid()) {
            capturedOn = earliestExif.toString(Qt::ISODate);
            dateSource = QStringLiteral("exif");
        }

        if (!capturedOn.isEmpty() || !captureExisted) {
            QSqlQuery setDate(db);
            setDate.prepare(QStringLiteral(
                "UPDATE capture SET captured_on = ?, date_source = ? WHERE id = ?"));
            setDate.addBindValue(capturedOn.isEmpty() ? QVariant() : capturedOn);
            setDate.addBindValue(dateSource);
            setDate.addBindValue(captureId);
            if (!setDate.exec())
                return bail(setDate.lastError().text());
        }

        ++m_stats.capturesSeen;
        if (m_progress && (m_stats.capturesSeen % 200) == 0)
            m_progress(m_stats);
    }

    if (!db.commit())
        return bail(QStringLiteral("commit failed: %1").arg(db.lastError().text()));

    summary.foldersUpserted = m_folderIds.size();
    return summary;
}

} // namespace pl::scan
