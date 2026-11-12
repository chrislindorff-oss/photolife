#pragma once

#include <QDate>
#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>

#include <atomic>
#include <functional>

namespace pl::organise {

// Import from Card: copies the photos on a memory card (or any folder) into
// download folders on the computer, one per day they were taken --
// "<destination>/2026-11-13" or "<destination>/2026-11-13 Anakie" -- ready
// to sort in Organise. Each copy is checked against the original byte for
// byte (SHA-256) before it counts, and keeps the original's date. The card is
// only ever read: nothing on it is written, moved or deleted.
//
// Shots imported before (by an earlier import from this card or another) are
// recognised by name, size and date in a small log kept beside the
// destination, and skipped. No widgets, no catalogue: safe on a worker thread.
class CardImporter
{
public:
    static constexpr const char *kLogFile = ".photolife-imports.json";

    struct Source
    {
        QString path;          // on the card
        QString relative;      // from the folder imported (for messages)
        qint64 size = 0;
        QDateTime modified;
        QDate taken;           // the day it was taken (EXIF, else the file's date)
        bool alreadyImported = false;
    };

    // What's on the card: photos (camera RAW and JPEG/TIFF/PNG) and their
    // sidecars, in every folder under `root`, with the day each was taken
    // and whether the log says it's been imported. A sidecar goes with its
    // photo's day.
    static QList<Source> survey(const QString &root, const QString &destination,
                                const std::atomic<bool> *cancel = nullptr);

    // The folder for one day's photos: "2026-11-13", or "2026-11-13 Anakie".
    static QString folderName(const QDate &day, const QString &name);

    struct Result
    {
        int copied = 0;
        int skipped = 0;            // already imported, or already in the folder identical
        QStringList failures;       // "DCIM/100NIKON/DSC_1.NEF: why"
        QStringList folders;        // the day folders written to, oldest first
        bool cancelled = false;
        bool ok() const { return failures.isEmpty() && !cancelled; }
    };

    // Copies `files` (from survey(); those already imported are skipped
    // unless `again`) into day folders under `destination`. `progress`
    // reports (files done, total). A file whose name is already taken in the
    // day folder by a different file is copied under a new name
    // ("DSC_1234-2.NEF"); one that's there and identical is left.
    static Result import(const QList<Source> &files, const QString &destination, const QString &name,
                         bool again = false, const std::function<void(int, int)> &progress = {},
                         const std::atomic<bool> *cancel = nullptr);

    // Whether `path` looks like a camera card: it has a DCIM folder.
    static bool looksLikeCard(const QString &path);
    // Mounted volumes that look like camera cards (their root paths).
    static QStringList cardsMounted();
};

} // namespace pl::organise
