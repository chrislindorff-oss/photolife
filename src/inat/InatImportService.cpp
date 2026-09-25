#include "inat/InatImportService.h"

#include "inat/InatPhotoDownloader.h"
#include "util/PathSanitize.h"

#include <QDate>
#include <QDir>
#include <QLoggingCategory>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QUrl>

namespace pl::inat {
namespace {

using pl::util::sanitizeFilenameComponent;

// "SpeciesName - Place - DDMMYYYY", each part sanitized and omitted when the
// item has no data for it (place_guess and the observed-on date are both
// sometimes empty). Falls back to the old inat-<observation>-<photo> scheme
// when nothing else is available, so a fully unidentified/dateless/placeless
// photo still gets a stable, unique name.
QString baseFileName(const ImportItem &item)
{
    QStringList parts;
    if (const QString species = sanitizeFilenameComponent(item.taxonName); !species.isEmpty())
        parts << species;
    if (const QString place = sanitizeFilenameComponent(item.placeGuess); !place.isEmpty())
        parts << place;
    if (const QDate date = QDate::fromString(item.observedOn, Qt::ISODate); date.isValid())
        parts << date.toString(QStringLiteral("ddMMyyyy"));

    if (parts.isEmpty())
        return QStringLiteral("inat-%1-%2").arg(item.observationId).arg(item.photoId);
    return parts.join(QStringLiteral(" - "));
}

} // namespace

InatImportService::InatImportService(InatPhotoDownloader &downloader, QString authorName,
                                     QObject *parent)
    : QObject(parent), m_downloader(downloader), m_authorName(std::move(authorName))
{
}

void InatImportService::start(const QList<ImportItem> &items, const QString &destFolder)
{
    if (m_running)
        return;
    m_running = true;
    m_cancelled = false;
    m_destFolder = destFolder;
    m_items = items;
    m_index = 0;
    m_saved.clear();
    m_usedFileNames.clear();

    emit progress(0, m_items.size());
    importNext();
}

void InatImportService::importNext()
{
    if (m_cancelled) {
        m_running = false;
        emit finished(false, QStringLiteral("cancelled"), m_saved);
        return;
    }
    if (m_index >= m_items.size()) {
        m_running = false;
        emit finished(true, QString(), m_saved);
        return;
    }

    const ImportItem item = m_items.at(m_index);
    const QString base = baseFileName(item);
    QString fileName = base + QStringLiteral(".jpg");
    // Several photos from the same observation, sharing the same species,
    // place, and date, would otherwise all resolve to the same base name.
    for (int n = 2; m_usedFileNames.contains(fileName); ++n)
        fileName = QStringLiteral("%1 - %2.jpg").arg(base).arg(n);
    m_usedFileNames.insert(fileName);
    const QString destPath = QDir(m_destFolder).filePath(fileName);

    m_downloader.download(
        QUrl(item.downloadUrl), destPath, [this, item, destPath](bool ok, const QString &error) {
            if (!ok) {
                m_running = false;
                emit finished(false, error, m_saved);
                return;
            }

            ExifFields fields;
            fields.author = m_authorName;
            fields.dateTimeOriginal = item.observedOn;
            fields.latitude = item.latitude;
            fields.longitude = item.longitude;
            fields.taxonName = item.taxonName;
            QString exifError = m_exifWriter ? QString() : QStringLiteral("no EXIF writer configured");
            const bool exifOk = m_exifWriter && m_exifWriter(destPath, fields, &exifError);
            if (!exifOk) {
                // The downloaded photo is still worth having even if writing
                // its metadata failed -- don't discard the file over this.
                qWarning("Could not write EXIF for %s: %s", qPrintable(destPath),
                        qPrintable(exifError));
            }

            m_saved.append({destPath, item.observationId, item.photoId});
            ++m_index;
            emit progress(m_index, m_items.size());
            importNext();
        });
}

int InatImportService::stampProvenance(const QString &connectionName,
                                       const QList<SavedFile> &saved)
{
    int updated = 0;
    QSqlDatabase db = QSqlDatabase::database(connectionName, false);
    for (const SavedFile &s : saved) {
        QSqlQuery q(db);
        q.prepare(QStringLiteral(
            "UPDATE capture SET inat_observation_id = ?, inat_photo_id = ? "
            "WHERE id = (SELECT capture_id FROM rendition WHERE path = ? LIMIT 1)"));
        q.addBindValue(s.observationId);
        q.addBindValue(s.photoId);
        q.addBindValue(s.path);
        if (q.exec() && q.numRowsAffected() > 0)
            ++updated;
    }
    return updated;
}

QList<qint64> InatImportService::captureIdsFor(const QString &connectionName,
                                              const QList<SavedFile> &saved)
{
    QList<qint64> ids;
    QSqlQuery q(QSqlDatabase::database(connectionName, false));
    q.prepare(QStringLiteral("SELECT capture_id FROM rendition WHERE path = ? LIMIT 1"));
    for (const SavedFile &s : saved) {
        q.bindValue(0, s.path);
        if (q.exec() && q.next()) {
            const qint64 id = q.value(0).toLongLong();
            if (!ids.contains(id))
                ids.append(id);
        }
    }
    return ids;
}

} // namespace pl::inat
