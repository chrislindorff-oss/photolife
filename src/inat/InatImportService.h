#pragma once

#include "inat/ExifWriter.h"

#include <QList>
#include <QMetaType>
#include <QObject>
#include <QSet>
#include <QString>

#include <functional>
#include <optional>

namespace pl::inat {

class InatPhotoDownloader;

// One selected photo to import, everything InatImportService needs to
// download it and write its EXIF.
struct ImportItem
{
    qint64 observationId = 0;
    qint64 photoId = 0;
    QString downloadUrl;
    QString observedOn;                 // "YYYY-MM-DD", from the observation
    std::optional<double> latitude;
    std::optional<double> longitude;
    QString taxonName;
    QString placeGuess;                 // iNat's own free-text place description, may be empty
};

// One photo actually written to disk, and the iNat ids it came from -- what
// stampProvenance() needs once the destination folder has been scanned.
struct SavedFile
{
    QString path;
    qint64 observationId = 0;
    qint64 photoId = 0;
};

// Downloads a chosen set of iNat photos into `destFolder` one at a time,
// writing EXIF into each via ExifWriter. Does NOT add the folder to the
// library or stamp provenance on the resulting capture rows itself -- both
// need the caller's help: adding the folder to Settings::watchedRoots() and
// scanning it is MainWindow's job (the same tail addWatchedFolder() already
// uses), and provenance can only be stamped once that scan actually creates
// the capture rows, so call stampProvenance() with this run's `finished`
// payload only after that scan settles.
class InatImportService : public QObject
{
    Q_OBJECT

public:
    using ExifWriterFn = std::function<bool(const QString &path, const ExifFields &fields,
                                            QString *error)>;

    InatImportService(InatPhotoDownloader &downloader, QString authorName,
                      QObject *parent = nullptr);

    // Not defaulted to inat::writeExif() on purpose: that keeps this class
    // (and anything linking it, including its own unit tests) free of any
    // dependency on the real exiv2-backed writer unless a caller explicitly
    // wires one in -- production code (MainWindow) passes inat::writeExif
    // here; tests substitute a fake. Left unset, a downloaded photo is simply
    // saved without embedded metadata rather than crashing.
    void setExifWriter(ExifWriterFn writer) { m_exifWriter = std::move(writer); }

    void start(const QList<ImportItem> &items, const QString &destFolder);
    bool isRunning() const { return m_running; }

    // UPDATEs each newly-scanned capture (matched by its rendition's exact
    // file path) with the iNat observation/photo id it came from. Returns the
    // number of capture rows stamped.
    static int stampProvenance(const QString &connectionName, const QList<SavedFile> &saved);

signals:
    void progress(int done, int total);
    void finished(bool ok, const QString &error, QList<SavedFile> saved);

private:
    void importNext();

    InatPhotoDownloader &m_downloader;
    QString m_authorName;
    ExifWriterFn m_exifWriter;

    bool m_running = false;
    QString m_destFolder;
    QList<ImportItem> m_items;
    int m_index = 0;
    QList<SavedFile> m_saved;
    QSet<QString> m_usedFileNames;   // disambiguates same-observation photos this run
};

} // namespace pl::inat

Q_DECLARE_METATYPE(pl::inat::SavedFile)
