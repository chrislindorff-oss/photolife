#include "organise/OrganiseSession.h"

#include "raw/RawPreview.h"
#include "scan/Exif.h"
#include "scan/FileScanner.h"
#include "taxonomy/TaxonomyStore.h"
#include "util/PathSanitize.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <algorithm>

namespace pl::organise {

namespace {

// When the shot was taken and where, from its renditions' metadata: the
// JPEG's EXIF first, then the RAW's. The RAW's GPS comes from LibRaw's own
// parse -- the embedded JPEG's copy of it has garbled refs.
struct Metadata
{
    QDateTime taken;
    std::optional<double> latitude;
    std::optional<double> longitude;
};

Metadata readMetadata(const scan::DiscoveredCapture &capture)
{
    Metadata m;
    for (const scan::DiscoveredFile &f : capture.files) {
        if (!f.isRaw) {
            const scan::ExifData exif = scan::readJpegExif(f.path);
            if (!m.taken.isValid() && exif.hasDate())
                m.taken = exif.dateTimeOriginal;
            if (!m.latitude && exif.hasGps()) {
                m.latitude = exif.latitude;
                m.longitude = exif.longitude;
            }
        }
    }
    if (m.taken.isValid() && m.latitude)
        return m;
    for (const scan::DiscoveredFile &f : capture.files) {
        if (!f.isRaw || !raw::isAvailable())
            continue;
        if (!m.taken.isValid()) {
            QByteArray bytes = raw::extractEmbeddedJpegBytes(f.path);
            QBuffer buffer(&bytes);
            if (buffer.open(QIODevice::ReadOnly)) {
                const scan::ExifData exif = scan::readJpegExif(buffer);
                if (exif.hasDate())
                    m.taken = exif.dateTimeOriginal;
            }
        }
        if (!m.latitude) {
            const raw::RawGps gps = raw::extractGps(f.path);
            if (gps.latitude && gps.longitude) {
                m.latitude = gps.latitude;
                m.longitude = gps.longitude;
            }
        }
    }
    return m;
}

QString folded(const QString &name)
{
    return taxonomy::TaxonomyStore::foldName(name.trimmed());
}

} // namespace

int OrganisePlan::photos() const
{
    int n = 0;
    for (const FolderMove &f : folders)
        n += int(f.items.size());
    return n;
}

QList<Item> OrganiseSession::scanFolder(const QString &folder,
                                        const std::function<void(int, int)> &progress)
{
    const QString dir = QDir(folder).absolutePath();
    QList<Item> items;
    QSet<QString> claimed;   // files that belong to a shot
    scan::FileScanner scanner;
    if (progress)
        progress(0, 0);
    const QList<scan::DiscoveredCapture> captures = scanner.scan({dir});
    QList<scan::DiscoveredCapture> topLevel;
    for (const scan::DiscoveredCapture &capture : captures) {
        if (QDir(capture.dirPath).absolutePath() == dir)
            topLevel << capture;
    }
    if (progress)
        progress(0, int(topLevel.size()));

    int done = 0;
    for (const scan::DiscoveredCapture &c : topLevel) {
        Item item;
        item.baseName = c.baseName;
        QList<scan::DiscoveredFile> files = c.files;
        std::sort(files.begin(), files.end(), [](const auto &a, const auto &b) {
            return a.isRaw != b.isRaw ? a.isRaw : a.ext < b.ext;   // RAW first
        });
        for (const scan::DiscoveredFile &f : files) {
            const QString path = QDir(f.path).absolutePath();
            item.files << path;
            item.kinds << f.ext.toUpper();
            claimed.insert(QFileInfo(path).fileName());
            if (!f.isRaw && item.previewPath.isEmpty())
                item.previewPath = path;
        }
        if (item.previewPath.isEmpty() && !item.files.isEmpty())
            item.previewPath = item.files.first();
        item.size = files.isEmpty() ? 0 : files.first().size;
        item.mtime = files.isEmpty() ? 0 : files.first().mtime;
        const Metadata meta = readMetadata(c);
        item.taken = meta.taken;
        item.takenFromExif = item.taken.isValid();
        item.latitude = meta.latitude;
        item.longitude = meta.longitude;
        if (!item.taken.isValid())
            item.taken = QDateTime::fromSecsSinceEpoch(item.mtime);
        items << item;
        if (progress)
            progress(++done, int(topLevel.size()));
    }

    // Sidecars: "DSC_1234.xmp", "DSC_1234.NEF.xmp", "DSC_1234.NEF.dop"...
    QHash<QString, int> byBase;
    for (int i = 0; i < items.size(); ++i)
        byBase.insert(items.at(i).baseName, i);
    const QFileInfoList entries = QDir(dir).entryInfoList(QDir::Files | QDir::Hidden);
    for (const QFileInfo &fi : entries) {
        const QString name = fi.fileName();
        if (claimed.contains(name) || name.startsWith(QLatin1String(".photolife-")))
            continue;
        const int dot = name.indexOf(QLatin1Char('.'));
        if (dot <= 0)
            continue;
        // The longest base name the file name starts with, followed by a dot.
        QString base = name.left(dot);
        for (int at = dot; at >= 0 && !byBase.contains(base); at = name.indexOf(QLatin1Char('.'), at + 1))
            base = name.left(at);
        if (const auto it = byBase.constFind(base); it != byBase.constEnd()
            && name.startsWith(base + QLatin1Char('.')))
            items[*it].files << fi.absoluteFilePath();
    }

    std::stable_sort(items.begin(), items.end(), [](const Item &a, const Item &b) {
        if (a.taken != b.taken)
            return a.taken < b.taken;
        return a.baseName < b.baseName;
    });
    return items;
}

void OrganiseSession::setItems(const QString &folder, QList<Item> items)
{
    m_folder = QDir(folder).absolutePath();
    m_items = std::move(items);
    m_names.clear();
    m_rejected.clear();
    m_namingEnds.clear();
    m_nameOrder.clear();
}

void OrganiseSession::setName(int i, const QString &name)
{
    if (i < 0 || i >= count())
        return;
    const QString trimmed = name.simplified();
    if (trimmed.isEmpty()) {
        m_names.remove(i);
        return;
    }
    m_names.insert(i, trimmed);
    m_nameOrder.removeAll(trimmed);
    m_nameOrder.prepend(trimmed);
}

int OrganiseSession::runStart(int i) const
{
    const int last = std::min(i, count() - 1);
    int lowerBound = -1;
    for (int boundary : m_namingEnds) {
        if (boundary < last)
            lowerBound = std::max(lowerBound, boundary);
    }
    for (int j = last; j > lowerBound; --j) {
        if (m_names.contains(j))
            return j;
    }
    return -1;
}

QString OrganiseSession::runName(int i) const
{
    const int start = runStart(i);
    return start < 0 ? QString() : m_names.value(start);
}

int OrganiseSession::runNumber(int i) const
{
    const int start = runStart(i);
    if (start < 0)
        return -1;
    int n = 0;
    for (auto it = m_names.cbegin(); it != m_names.cend(); ++it) {
        if (it.key() < start)
            ++n;
    }
    return n;
}

void OrganiseSession::setRejected(int i, bool rejected)
{
    if (i < 0 || i >= count())
        return;
    if (rejected)
        m_rejected.insert(i);
    else
        m_rejected.remove(i);
}

void OrganiseSession::setNamingEndAt(int i, bool ends)
{
    if (i < 0 || i >= count())
        return;
    if (ends)
        m_namingEnds.insert(i);
    else
        m_namingEnds.remove(i);
}

QStringList OrganiseSession::recentNames() const
{
    QStringList out;
    QSet<QString> seen;
    for (const QString &n : m_nameOrder) {
        const QString key = folded(n);
        bool stillUsed = false;
        for (const QString &v : m_names) {
            if (folded(v) == key)
                stillUsed = true;
        }
        if (stillUsed && !seen.contains(key)) {
            seen.insert(key);
            out << n;
        }
    }
    return out;
}

QString OrganiseSession::folderNameFor(const QString &name)
{
    QString out = util::sanitizeFilenameComponent(name.simplified()).simplified();
    // Windows can't have a folder name ending in a dot or a space.
    while (out.endsWith(QLatin1Char('.')) || out.endsWith(QLatin1Char(' ')))
        out.chop(1);
    return out;
}

OrganisePlan OrganiseSession::plan() const
{
    OrganisePlan plan;
    QHash<QString, int> folderIndex;   // folded name -> plan.folders index
    for (int i = 0; i < count(); ++i) {
        if (m_rejected.contains(i)) {
            plan.rejects << i;
            continue;
        }
        const QString name = runName(i);
        if (name.isEmpty()) {
            ++plan.unnamed;
            continue;
        }
        const QString key = folded(name);
        auto it = folderIndex.find(key);
        if (it == folderIndex.end()) {
            it = folderIndex.insert(key, int(plan.folders.size()));
            plan.folders.append({folderNameFor(name), {}});
        }
        plan.folders[*it].items << i;
    }
    return plan;
}

bool OrganiseSession::saveProgress() const
{
    if (m_folder.isEmpty())
        return false;
    const QString path = QDir(m_folder).filePath(QLatin1String(kProgressFile));
    // Shots no longer in the folder (organised into a subfolder) keep their
    // entries, so undoing an organise brings their names back too.
    QJsonObject shots;
    {
        QFile old(path);
        if (old.open(QIODevice::ReadOnly))
            shots = QJsonDocument::fromJson(old.readAll()).object().value(QStringLiteral("shots")).toObject();
        for (const Item &item : m_items)
            shots.remove(item.baseName);
    }
    for (int i = 0; i < count(); ++i) {
        if (!m_names.contains(i) && !m_rejected.contains(i) && !m_namingEnds.contains(i))
            continue;
        const Item &item = m_items.at(i);
        QJsonObject o{{QStringLiteral("size"), double(item.size)},
                      {QStringLiteral("mtime"), double(item.mtime)}};
        if (m_names.contains(i))
            o.insert(QStringLiteral("name"), m_names.value(i));
        if (m_rejected.contains(i))
            o.insert(QStringLiteral("rejected"), true);
        if (m_namingEnds.contains(i))
            o.insert(QStringLiteral("namingEnd"), true);
        shots.insert(item.baseName, o);
    }
    if (shots.isEmpty()) {
        QFile::remove(path);   // nothing to keep
        return true;
    }
    QJsonArray order;
    for (const QString &n : m_nameOrder)
        order.append(n);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(QJsonDocument(QJsonObject{{QStringLiteral("version"), 1},
                                         {QStringLiteral("shots"), shots},
                                         {QStringLiteral("recent"), order}})
                   .toJson());
    return file.commit();
}

void OrganiseSession::loadProgress()
{
    QFile file(QDir(m_folder).filePath(QLatin1String(kProgressFile)));
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    const QJsonObject shots = root.value(QStringLiteral("shots")).toObject();
    for (int i = 0; i < count(); ++i) {
        const Item &item = m_items.at(i);
        const QJsonObject o = shots.value(item.baseName).toObject();
        if (o.isEmpty() || qint64(o.value(QStringLiteral("size")).toDouble()) != item.size
            || qint64(o.value(QStringLiteral("mtime")).toDouble()) != item.mtime)
            continue;   // not the same shot any more
        const QString name = o.value(QStringLiteral("name")).toString();
        if (!name.isEmpty())
            m_names.insert(i, name);
        if (o.value(QStringLiteral("rejected")).toBool())
            m_rejected.insert(i);
        if (o.value(QStringLiteral("namingEnd")).toBool())
            m_namingEnds.insert(i);
    }
    for (const QJsonValue &v : root.value(QStringLiteral("recent")).toArray())
        m_nameOrder << v.toString();
}

void OrganiseSession::clearProgress()
{
    m_names.clear();
    m_rejected.clear();
    m_namingEnds.clear();
    m_nameOrder.clear();
    QFile::remove(QDir(m_folder).filePath(QLatin1String(kProgressFile)));
}

} // namespace pl::organise
