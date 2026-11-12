#include "organise/CardImporter.h"

#include "scan/Exif.h"
#include "scan/FileScanner.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStorageInfo>

#include <algorithm>

namespace pl::organise {
namespace {

bool isPhoto(const QString &suffix)
{
    return scan::FileScanner::rawExtensions().contains(suffix) || scan::FileScanner::imageExtensions().contains(suffix);
}

bool isSidecar(const QString &suffix)
{
    return suffix == QLatin1String("xmp");
}

// A shot as the log knows it: its name, size and date on the card.
QString logKey(const QString &fileName, qint64 size, const QDateTime &modified)
{
    return QStringLiteral("%1|%2|%3").arg(fileName).arg(size).arg(modified.toSecsSinceEpoch());
}

QString logPath(const QString &destination)
{
    return QDir(destination).filePath(QLatin1String(CardImporter::kLogFile));
}

QJsonObject readLog(const QString &destination)
{
    QFile file(logPath(destination));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}

QByteArray sha256(const QString &path, bool *ok)
{
    QFile file(path);
    *ok = file.open(QIODevice::ReadOnly);
    if (!*ok)
        return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    *ok = hash.addData(&file);
    return hash.result();
}

// Copies `from` to `to` through a ".part" file that only takes its name once
// it reads back the same as the original; `to` must not exist. The copy keeps
// the original's date.
bool verifiedCopy(const QString &from, const QString &to, QString *why)
{
    const QString part = to + QStringLiteral(".photolife-part");
    QFile::remove(part);   // a leftover from an import that was stopped
    QFile in(from);
    if (!in.open(QIODevice::ReadOnly)) {
        *why = QStringLiteral("couldn't be read from the card (%1)").arg(in.errorString());
        return false;
    }
    QFile out(part);
    if (!out.open(QIODevice::WriteOnly)) {
        *why = QStringLiteral("couldn't be written (%1)").arg(out.errorString());
        return false;
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QByteArray buffer;
    while (!(buffer = in.read(4 * 1024 * 1024)).isEmpty()) {
        hash.addData(buffer);
        if (out.write(buffer) != buffer.size()) {
            *why = QStringLiteral("couldn't be written (%1)").arg(out.errorString());
            out.close();
            QFile::remove(part);
            return false;
        }
    }
    const bool readError = in.error() != QFileDevice::NoError;
    const QDateTime modified = QFileInfo(from).lastModified();
    if (!out.flush() || readError) {
        *why = readError ? QStringLiteral("the card stopped being readable part-way (%1)").arg(in.errorString())
                         : QStringLiteral("couldn't be written (%1)").arg(out.errorString());
        out.close();
        QFile::remove(part);
        return false;
    }
    out.setFileTime(modified, QFileDevice::FileModificationTime);
    out.close();
    // Read back what landed on the disk, not what was meant to.
    bool ok = false;
    if (sha256(part, &ok) != hash.result() || !ok) {
        *why = QStringLiteral("the copy didn't match the original, so it was discarded");
        QFile::remove(part);
        return false;
    }
    if (!QFile::rename(part, to)) {
        *why = QStringLiteral("couldn't be given its name");
        QFile::remove(part);
        return false;
    }
    return true;
}

} // namespace

QString CardImporter::folderName(const QDate &day, const QString &name)
{
    QString n = name.simplified();
    // Nothing a file system would refuse.
    static const QString bad = QStringLiteral("/\\:*?\"<>|");
    for (QChar &c : n) {
        if (bad.contains(c) || c.unicode() < 32)
            c = QLatin1Char('-');
    }
    const QString date = day.isValid() ? day.toString(Qt::ISODate) : QStringLiteral("Undated");
    return n.isEmpty() ? date : date + QLatin1Char(' ') + n;
}

QList<CardImporter::Source> CardImporter::survey(const QString &root, const QString &destination,
                                                 const std::atomic<bool> *cancel)
{
    QList<Source> photos;
    QList<Source> sidecars;
    const QDir base(root);
    QDirIterator it(root, QDir::Files | QDir::Readable, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        if (cancel && *cancel)
            return {};
        it.next();
        const QFileInfo info = it.fileInfo();
        const QString suffix = info.suffix().toLower();
        if (info.fileName().startsWith(QLatin1Char('.')) || (!isPhoto(suffix) && !isSidecar(suffix)))
            continue;
        Source s;
        s.path = info.absoluteFilePath();
        s.relative = base.relativeFilePath(s.path);
        s.size = info.size();
        s.modified = info.lastModified();
        (isPhoto(suffix) ? photos : sidecars) << s;
    }
    // The day each was taken: its EXIF date, else its file's.
    QHash<QString, QDate> dayByStem;   // folder + base name -> day, for the sidecars
    for (Source &s : photos) {
        if (cancel && *cancel)
            return {};
        const scan::ExifData exif = scan::readPhotoExif(s.path);
        s.taken = exif.hasDate() ? exif.dateTimeOriginal.date() : s.modified.date();
        const QFileInfo info(s.path);
        dayByStem.insert(info.absolutePath() + QLatin1Char('/') + info.completeBaseName().section(QLatin1Char('.'), 0, 0),
                         s.taken);
    }
    for (Source &s : sidecars) {
        const QFileInfo info(s.path);
        // DSC_1234.xmp and DSC_1234.NEF.xmp both belong to DSC_1234.
        const QString stem = info.absolutePath() + QLatin1Char('/') + info.fileName().section(QLatin1Char('.'), 0, 0);
        s.taken = dayByStem.value(stem, s.modified.date());
    }
    QList<Source> all = photos + sidecars;
    const QJsonObject imported = readLog(destination).value(QStringLiteral("imported")).toObject();
    for (Source &s : all)
        s.alreadyImported = imported.contains(logKey(QFileInfo(s.path).fileName(), s.size, s.modified));
    std::sort(all.begin(), all.end(), [](const Source &a, const Source &b) {
        return a.taken != b.taken ? a.taken < b.taken : a.relative < b.relative;
    });
    return all;
}

CardImporter::Result CardImporter::import(const QList<Source> &files, const QString &destination,
                                          const QString &name, bool again,
                                          const std::function<void(int, int)> &progress,
                                          const std::atomic<bool> *cancel)
{
    Result out;
    QList<const Source *> todo;
    for (const Source &s : files) {
        if (s.alreadyImported && !again)
            ++out.skipped;
        else
            todo << &s;
    }
    QJsonObject root = readLog(destination);
    QJsonObject imported = root.value(QStringLiteral("imported")).toObject();
    auto saveLog = [&] {
        root.insert(QStringLiteral("version"), 1);
        root.insert(QStringLiteral("imported"), imported);
        QSaveFile log(logPath(destination));
        if (log.open(QIODevice::WriteOnly)) {
            log.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
            log.commit();
        }
    };
    if (!QDir().mkpath(destination)) {
        out.failures << QStringLiteral("%1: the folder couldn't be made").arg(destination);
        return out;
    }
    const int total = int(todo.size());
    for (int i = 0; i < total; ++i) {
        if (cancel && *cancel) {
            out.cancelled = true;
            break;
        }
        const Source &s = *todo.at(i);
        const QString folder = QDir(destination).filePath(folderName(s.taken, name));
        if (!out.folders.contains(folder)) {
            if (!QDir().mkpath(folder)) {
                out.failures << QStringLiteral("%1: the folder %2 couldn't be made").arg(s.relative, folder);
                continue;
            }
            out.folders << folder;
        }
        const QFileInfo info(s.path);
        QString target = QDir(folder).filePath(info.fileName());
        bool identical = false;
        // A name already taken in the folder: the same photo (left), or another (a new name).
        for (int n = 2; QFileInfo::exists(target); ++n) {
            if (QFileInfo(target).size() == s.size) {
                bool okA = false, okB = false;
                if (sha256(target, &okA) == sha256(s.path, &okB) && okA && okB) {
                    identical = true;
                    break;
                }
            }
            const QString suffix = info.fileName().section(QLatin1Char('.'), 1);
            target = QDir(folder).filePath(QStringLiteral("%1-%2.%3")
                                               .arg(info.fileName().section(QLatin1Char('.'), 0, 0))
                                               .arg(n)
                                               .arg(suffix));
        }
        QString why;
        if (identical) {
            ++out.skipped;
        } else if (verifiedCopy(s.path, target, &why)) {
            ++out.copied;
        } else {
            out.failures << QStringLiteral("%1: %2").arg(s.relative, why);
            if (progress)
                progress(i + 1, total);
            continue;
        }
        imported.insert(logKey(info.fileName(), s.size, s.modified),
                        QJsonObject{{QStringLiteral("to"), QDir(destination).relativeFilePath(target)},
                                    {QStringLiteral("on"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}});
        if ((i + 1) % 50 == 0)
            saveLog();   // a card pulled out part-way still knows what came off it
        if (progress)
            progress(i + 1, total);
    }
    saveLog();
    std::sort(out.folders.begin(), out.folders.end());
    return out;
}

bool CardImporter::looksLikeCard(const QString &path)
{
    return QDir(path).exists(QStringLiteral("DCIM"));
}

QStringList CardImporter::cardsMounted()
{
    QStringList out;
    for (const QStorageInfo &volume : QStorageInfo::mountedVolumes()) {
        if (!volume.isValid() || !volume.isReady() || volume.isRoot())
            continue;
        if (looksLikeCard(volume.rootPath()))
            out << volume.rootPath();
    }
    return out;
}

} // namespace pl::organise
