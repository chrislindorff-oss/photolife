#include "scan/FileScanner.h"

#include <QDir>
#include <QFileInfo>

#include <algorithm>

namespace pl::scan {

const QSet<QString> &FileScanner::imageExtensions()
{
    static const QSet<QString> exts = {
        QStringLiteral("jpg"), QStringLiteral("jpeg"), QStringLiteral("jpe"),
        QStringLiteral("png"), QStringLiteral("tif"),  QStringLiteral("tiff"),
    };
    return exts;
}

const QSet<QString> &FileScanner::rawExtensions()
{
    static const QSet<QString> exts = {
        QStringLiteral("nef"), QStringLiteral("nrw"), QStringLiteral("cr2"),
        QStringLiteral("cr3"), QStringLiteral("crw"), QStringLiteral("arw"),
        QStringLiteral("raf"), QStringLiteral("orf"), QStringLiteral("rw2"),
        QStringLiteral("dng"), QStringLiteral("pef"), QStringLiteral("srw"),
        QStringLiteral("raw"), QStringLiteral("3fr"), QStringLiteral("iiq"),
    };
    return exts;
}

bool FileScanner::isIgnoredFile(const QString &fileName)
{
    if (fileName.isEmpty() || fileName.startsWith(QLatin1Char('.')))
        return true;

    static const QSet<QString> names = {
        QStringLiteral("thumbs.db"),   QStringLiteral("sjthumbs.dat"),
        QStringLiteral("desktop.ini"), QStringLiteral("picasa.ini"),
        QStringLiteral(".picasa.ini"), QStringLiteral("zbthumbnail.info"),
    };
    if (names.contains(fileName.toLower()))
        return true;

    static const QSet<QString> ignoredExts = {
        QStringLiteral("nksc"), QStringLiteral("xmp"), QStringLiteral("ini"),
        QStringLiteral("dat"),  QStringLiteral("db"),  QStringLiteral("rar"),
        QStringLiteral("zip"),  QStringLiteral("txt"), QStringLiteral("dtstyle"),
        QStringLiteral("info"), QStringLiteral("thm"), QStringLiteral("pp3"),
    };
    return ignoredExts.contains(QFileInfo(fileName).suffix().toLower());
}

bool FileScanner::isIgnoredDir(const QString &dirName)
{
    if (dirName.isEmpty() || dirName.startsWith(QLatin1Char('.')))
        return true;

    static const QSet<QString> names = {
        QStringLiteral("nksc_param"),        QStringLiteral("jpg_original"),
        QStringLiteral("temp gps"),          QStringLiteral("to sort and upload"),
        QStringLiteral("$recycle.bin"),      QStringLiteral("@eadir"),
        QStringLiteral("lightroom"),         QStringLiteral(".dtrash"),
    };
    return names.contains(dirName.toLower());
}

QList<DiscoveredCapture> FileScanner::scan(const QStringList &roots)
{
    m_stats = {};
    m_captures.clear();
    m_visitedDirs.clear();

    for (const QString &root : roots) {
        if (isCancelled())
            break;
        const QFileInfo info(root);
        if (info.isDir())
            scanDir(info.absoluteFilePath());
    }

    return m_captures;
}

void FileScanner::scanDir(const QString &dirPath)
{
    if (isCancelled())
        return;

    const QString canonical = QFileInfo(dirPath).canonicalFilePath();
    if (canonical.isEmpty() || m_visitedDirs.contains(canonical))
        return;
    m_visitedDirs.insert(canonical);

    ++m_stats.foldersSeen;
    m_stats.currentDir = dirPath;
    if (m_progress)
        m_progress(m_stats);

    QDir dir(dirPath);
    const QFileInfoList entries =
        dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);

    // Files in this directory, grouped by stem into captures.
    QList<DiscoveredCapture> here;
    auto captureForStem = [&](const QString &stem) -> DiscoveredCapture & {
        for (DiscoveredCapture &c : here) {
            if (c.baseName == stem)
                return c;
        }
        here.push_back({dirPath, stem, {}});
        return here.last();
    };

    QStringList subdirs;
    for (const QFileInfo &entry : entries) {
        if (isCancelled())
            return;

        if (entry.isDir()) {
            if (!isIgnoredDir(entry.fileName()))
                subdirs << entry.absoluteFilePath();
            continue;
        }

        const QString fileName = entry.fileName();
        if (isIgnoredFile(fileName))
            continue;

        const QString ext = entry.suffix().toLower();
        const bool isRaw = rawExtensions().contains(ext);
        if (!isRaw && !imageExtensions().contains(ext))
            continue;

        DiscoveredFile file;
        file.path = entry.absoluteFilePath();
        file.dirPath = dirPath;
        file.stem = entry.completeBaseName();
        file.ext = ext;
        file.size = entry.size();
        file.mtime = entry.lastModified().toSecsSinceEpoch();
        file.isRaw = isRaw;

        captureForStem(file.stem).files.push_back(file);
        ++m_stats.filesSeen;
    }

    for (DiscoveredCapture &c : here) {
        std::sort(c.files.begin(), c.files.end(),
                  [](const DiscoveredFile &a, const DiscoveredFile &b) {
                      if (a.isRaw != b.isRaw)
                          return !a.isRaw;   // JPEG-ish first, RAW second
                      return a.path < b.path;
                  });
        m_captures.push_back(c);
        ++m_stats.capturesSeen;
    }
    if (m_progress && !here.isEmpty())
        m_progress(m_stats);

    for (const QString &sub : subdirs) {
        if (isCancelled())
            return;
        scanDir(sub);
    }
}

} // namespace pl::scan
