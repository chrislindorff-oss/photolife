#include "organise/OrganiseSession.h"

#include "inat/ExifWriter.h"
#include "raw/RawPreview.h"
#include "scan/Exif.h"
#include "scan/FileScanner.h"
#include "taxonomy/TaxonomyStore.h"
#include "util/PathSanitize.h"

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

bool isRawFile(const QString &path)
{
    return scan::FileScanner::rawExtensions().contains(QFileInfo(path).suffix().toLower());
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

QList<Item> OrganiseSession::listFolder(const QString &folder)
{
    const QString dir = QDir(folder).absolutePath();
    QList<Item> items;
    QSet<QString> claimed;   // files that belong to a shot
    scan::FileScanner scanner;
    const QList<scan::DiscoveredCapture> captures = scanner.scan({dir});
    for (const scan::DiscoveredCapture &c : captures) {
        if (QDir(c.dirPath).absolutePath() != dir)
            continue;
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
        item.taken = QDateTime::fromSecsSinceEpoch(item.mtime);
        items << item;
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

    sortByTaken(items);
    return items;
}

void OrganiseSession::readMetadata(Item &item)
{
    // The renditions come first in item.files, RAW first; then sidecars.
    const QStringList renditions = item.files.mid(0, item.kinds.size());
    QDateTime taken;
    // The JPEG's EXIF first...
    for (const QString &path : renditions) {
        if (isRawFile(path))
            continue;
        const scan::ExifData exif = scan::readJpegExif(path);
        if (!taken.isValid() && exif.hasDate())
            taken = exif.dateTimeOriginal;
        if (!item.hasGps() && exif.hasGps()) {
            item.latitude = exif.latitude;
            item.longitude = exif.longitude;
        }
    }
    // ...then the RAW's own: read straight from the start of a TIFF-based RAW
    // (one small read, which matters on a slow disk), else through LibRaw.
    // (Not the embedded preview's EXIF: Nikon's has none, and LibRaw's copy
    // garbles the GPS refs.)
    for (const QString &path : renditions) {
        if (taken.isValid() && item.hasGps())
            break;
        if (!isRawFile(path))
            continue;
        const scan::ExifData exif = scan::readTiffExif(path);
        std::optional<double> latitude = exif.latitude, longitude = exif.longitude;
        QDateTime rawTaken = exif.dateTimeOriginal;
        if (!rawTaken.isValid() && raw::isAvailable()) {
            const raw::RawInfo info = raw::readInfo(path);
            rawTaken = info.taken;
            latitude = info.gps.latitude;
            longitude = info.gps.longitude;
        }
        if (!taken.isValid())
            taken = rawTaken;
        if (!item.hasGps() && latitude && longitude) {
            item.latitude = latitude;
            item.longitude = longitude;
        }
    }
    item.takenFromExif = taken.isValid();
    item.taken = taken.isValid() ? taken : QDateTime::fromSecsSinceEpoch(item.mtime);
}

void OrganiseSession::sortByTaken(QList<Item> &items)
{
    std::stable_sort(items.begin(), items.end(), [](const Item &a, const Item &b) {
        if (a.taken != b.taken)
            return a.taken < b.taken;
        return a.baseName < b.baseName;
    });
}

QList<Item> OrganiseSession::scanFolder(const QString &folder,
                                        const std::function<void(int, int)> &progress)
{
    if (progress)
        progress(0, 0);
    QList<Item> items = listFolder(folder);
    if (progress)
        progress(0, int(items.size()));
    for (int i = 0; i < items.size(); ++i) {
        readMetadata(items[i]);
        if (progress)
            progress(i + 1, int(items.size()));
    }
    sortByTaken(items);
    return items;
}

int OrganiseSession::firstPageSize(const QString &folder, const QList<Item> &listed)
{
    // Far enough to reach the last photo given a name, so carrying on picks
    // up where it left off.
    int last = -1;
    QFile file(QDir(folder).filePath(QLatin1String(kProgressFile)));
    if (file.open(QIODevice::ReadOnly)) {
        const QJsonObject shots =
            QJsonDocument::fromJson(file.readAll()).object().value(QStringLiteral("shots")).toObject();
        for (int i = 0; i < listed.size(); ++i) {
            if (!shots.value(listed.at(i).baseName).toObject().value(QStringLiteral("name")).toString().isEmpty())
                last = i;
        }
    }
    return int(std::min<qsizetype>(listed.size(), last + 1 + kPageSize));
}

void OrganiseSession::setItems(const QString &folder, QList<Item> items, QList<Item> pending)
{
    m_folder = QDir(folder).absolutePath();
    m_items = std::move(items);
    m_pending = std::move(pending);
    m_names.clear();
    m_rejected.clear();
    m_best.clear();
    m_namingEnds.clear();
    m_nameOrder.clear();
}

int OrganiseSession::appendItems(QList<Item> batch)
{
    // They're the front of the pending list, read now.
    const int from = count();
    const qsizetype n = std::min(batch.size(), m_pending.size());
    m_pending.remove(0, n);
    batch.resize(n);
    sortByTaken(batch);
    m_items += batch;
    applyProgress(from);
    return int(n);
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
    if (rejected) {
        m_rejected.insert(i);
        m_best.remove(i);
    } else {
        m_rejected.remove(i);
    }
}

void OrganiseSession::setBest(int i, bool best)
{
    if (i < 0 || i >= count())
        return;
    if (best) {
        m_best.insert(i);
        m_rejected.remove(i);
    } else {
        m_best.remove(i);
    }
}

QList<int> OrganiseSession::bestShots() const
{
    QList<int> out(m_best.cbegin(), m_best.cend());
    std::sort(out.begin(), out.end());
    return out;
}

void OrganiseSession::refreshFileStats(int i)
{
    if (i < 0 || i >= count() || m_items.at(i).files.isEmpty())
        return;
    const QFileInfo first(m_items.at(i).files.first());
    m_items[i].size = first.size();
    m_items[i].mtime = first.lastModified().toSecsSinceEpoch();
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

bool OrganiseSession::rateFiveStars(const Item &item, QString *createdSidecar, QString *error)
{
    // A JPEG holds its own rating; a RAW (or anything else) gets it in an XMP
    // sidecar -- its existing one if it has one -- and is never written to.
    QString sidecar;
    bool needsSidecar = false;
    for (const QString &path : item.files) {
        const QString ext = QFileInfo(path).suffix().toLower();
        if (ext == QLatin1String("xmp")) {
            if (sidecar.isEmpty())
                sidecar = path;
        } else if (ext == QLatin1String("jpg") || ext == QLatin1String("jpeg")) {
            const QDateTime modified = QFileInfo(path).lastModified();
            inat::ExifFields fields;
            fields.rating = 5;
            if (!inat::writeExifSafely(path, fields, error))
                return false;
            // Its date as it was: a rating isn't an edit.
            QFile file(path);
            if (file.open(QIODevice::ReadWrite))
                file.setFileTime(modified, QFileDevice::FileModificationTime);
        } else if (item.kinds.contains(QFileInfo(path).suffix().toUpper())) {
            needsSidecar = true;   // a RAW (or another kind) rendition
        }
    }
    if (!needsSidecar)
        return true;
    if (sidecar.isEmpty()) {
        sidecar = QFileInfo(item.files.first()).absoluteDir().filePath(item.baseName + QStringLiteral(".xmp"));
        if (createdSidecar && !QFileInfo::exists(sidecar))
            *createdSidecar = sidecar;
    }
    return inat::writeSidecarRating(sidecar, 5, error);
}

QString OrganiseSession::renamedBase(const QString &folderName, const QString &place,
                                     const QDateTime &taken)
{
    constexpr int kMaxStem = 200;   // room for " (2)" and an extension
    const QString date = taken.date().toString(Qt::ISODate);
    const QString where = util::sanitizeFilenameComponent(place.simplified()).simplified();
    QString out = folderNameFor(folderName);
    QString tail = where.isEmpty() ? date : where + QStringLiteral(", ") + date;
    if (out.size() + 3 + tail.size() > kMaxStem)
        out.truncate(std::max(0, int(kMaxStem - 3 - tail.size())));
    out = out.trimmed();
    if (!tail.isEmpty())
        out = out.isEmpty() ? tail : out + QStringLiteral(" - ") + tail;
    out.truncate(kMaxStem);
    return folderNameFor(out);
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
        if (!m_names.contains(i) && !m_rejected.contains(i) && !m_namingEnds.contains(i) && !m_best.contains(i))
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
        if (m_best.contains(i))
            o.insert(QStringLiteral("best"), true);
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
    const QJsonObject root = applyProgress(0);
    for (const QJsonValue &v : root.value(QStringLiteral("recent")).toArray())
        m_nameOrder << v.toString();
}

QJsonObject OrganiseSession::applyProgress(int from)
{
    QFile file(QDir(m_folder).filePath(QLatin1String(kProgressFile)));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    const QJsonObject shots = root.value(QStringLiteral("shots")).toObject();
    for (int i = from; i < count(); ++i) {
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
        if (o.value(QStringLiteral("best")).toBool())
            m_best.insert(i);
    }
    return root;
}

void OrganiseSession::clearProgress()
{
    m_names.clear();
    m_rejected.clear();
    m_best.clear();
    m_namingEnds.clear();
    m_nameOrder.clear();
    QFile::remove(QDir(m_folder).filePath(QLatin1String(kProgressFile)));
}

} // namespace pl::organise
