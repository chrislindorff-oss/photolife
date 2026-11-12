#include "organise/OrganiseSession.h"

#include "inat/ExifWriter.h"
#include "raw/RawPreview.h"
#include "scan/Exif.h"
#include "scan/FileScanner.h"
#include "taxonomy/TaxonomyStore.h"
#include "util/PathSanitize.h"

#include <QDir>
#include <QRegularExpression>
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
    // A location set on the map, or by another program, for a RAW: in its sidecar.
    if (!item.hasGps()) {
        for (const QString &path : item.files.mid(item.kinds.size())) {
            if (!path.endsWith(QLatin1String(".xmp"), Qt::CaseInsensitive))
                continue;
            const scan::ExifData xmp = scan::readXmpGps(path);
            if (xmp.hasGps()) {
                item.latitude = xmp.latitude;
                item.longitude = xmp.longitude;
                break;
            }
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
    m_unsure.clear();
    m_notes.clear();
    m_nameOrder.clear();
    m_gpxPath.clear();
    m_gpxOffsetSeconds = 0;
    m_gpxChosen = false;
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
        m_unsure.remove(i);
        m_notes.remove(i);
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

QList<int> OrganiseSession::proposedRunStarts(int gapSeconds) const
{
    QList<int> starts;
    int previous = -1;   // the last photo looked at that isn't a reject
    for (int i = 0; i < count(); ++i) {
        if (isRejected(i))
            continue;
        if (runName(i).isEmpty()) {
            const bool newStretch = previous < 0 || !runName(previous).isEmpty();
            const QDateTime &before = previous >= 0 ? m_items.at(previous).taken : QDateTime();
            const QDateTime &taken = m_items.at(i).taken;
            if (newStretch || !before.isValid() || !taken.isValid()
                || before.secsTo(taken) > gapSeconds)
                starts << i;
        }
        previous = i;
    }
    return starts;
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

QString RunNotes::summary() const
{
    QStringList parts;
    for (const QString &p : {sex, lifeStage, alive, flowers}) {
        if (!p.isEmpty())
            parts << p;
    }
    if (!text.trimmed().isEmpty())
        parts << text.simplified();
    return parts.join(QStringLiteral(" · "));
}

void OrganiseSession::setUnsure(int i, bool unsure)
{
    const int start = runStart(i);
    if (start < 0)
        return;
    if (unsure)
        m_unsure.insert(start);
    else
        m_unsure.remove(start);
}

int OrganiseSession::nextUnsure(int i) const
{
    if (m_unsure.isEmpty())
        return -1;
    QList<int> marked(m_unsure.cbegin(), m_unsure.cend());
    std::sort(marked.begin(), marked.end());
    for (int m : std::as_const(marked)) {
        if (m > i)
            return m;
    }
    return marked.first();
}

void OrganiseSession::setNotes(int i, const RunNotes &notes)
{
    const int start = runStart(i);
    if (start < 0)
        return;
    if (notes.isEmpty())
        m_notes.remove(start);
    else
        m_notes.insert(start, notes);
}

QList<Burst> OrganiseSession::bursts(int gapSeconds) const
{
    QList<Burst> out;
    int first = 0;
    for (int i = 1; i <= count(); ++i) {
        const bool continues = i < count() && m_items.at(i).taken.isValid() && m_items.at(i - 1).taken.isValid()
                               && m_items.at(i - 1).taken.secsTo(m_items.at(i).taken) <= gapSeconds
                               && m_items.at(i - 1).taken.secsTo(m_items.at(i).taken) >= 0;
        if (continues)
            continue;
        if (i - 1 > first)
            out.append({first, i - 1});
        first = i;
    }
    return out;
}

QString OrganiseSession::uncertainName(const QString &name)
{
    static const QRegularExpression binomial(QStringLiteral("^([A-Z][a-z-]+) ([a-z].*)$"));
    const QRegularExpressionMatch m = binomial.match(name.simplified());
    if (m.hasMatch())
        return QStringLiteral("%1 cf. %2").arg(m.captured(1), m.captured(2));
    return QStringLiteral("cf. ") + name.simplified();
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

geo::GeotagWriter::Result OrganiseSession::setLocation(int i, double latitude, double longitude, bool intoRaw)
{
    if (i < 0 || i >= count())
        return {};
    Item &item = m_items[i];
    geo::GeotagWriter::Result result = geo::GeotagWriter::writeShot(
        item.files.mid(0, item.kinds.size()), item.baseName, latitude, longitude, intoRaw);
    if (!result.createdSidecar.isEmpty() && !item.files.contains(result.createdSidecar))
        item.files << result.createdSidecar;   // it moves with the shot
    // What the files now hold: read back, so the place shown is the one written.
    Item reread = item;
    reread.latitude.reset();
    reread.longitude.reset();
    readMetadata(reread);
    item.latitude = reread.hasGps() ? reread.latitude : std::optional<double>(latitude);
    item.longitude = reread.hasGps() ? reread.longitude : std::optional<double>(longitude);
    refreshFileStats(i);
    saveProgress();
    return result;
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

bool OrganiseSession::writeNotes(const Item &item, const RunNotes &notes, QString *createdSidecar, QString *error)
{
    const QHash<QString, QString> fields{{QStringLiteral("sex"), notes.sex},
                                         {QStringLiteral("lifeStage"), notes.lifeStage},
                                         {QStringLiteral("alive"), notes.alive},
                                         {QStringLiteral("flowers"), notes.flowers},
                                         {QStringLiteral("notes"), notes.text.simplified()}};
    const QString description = notes.summary();
    QString sidecar;
    bool needsSidecar = false;
    for (const QString &path : item.files) {
        const QString ext = QFileInfo(path).suffix().toLower();
        if (ext == QLatin1String("xmp")) {
            if (sidecar.isEmpty())
                sidecar = path;
        } else if (ext == QLatin1String("jpg") || ext == QLatin1String("jpeg")) {
            const QDateTime modified = QFileInfo(path).lastModified();
            inat::ExifFields exif;
            exif.photolife = fields;
            exif.description = description;
            if (!inat::writeExifSafely(path, exif, error))
                return false;
            QFile file(path);
            if (file.open(QIODevice::ReadWrite))
                file.setFileTime(modified, QFileDevice::FileModificationTime);
        } else if (item.kinds.contains(QFileInfo(path).suffix().toUpper())) {
            needsSidecar = true;
        }
    }
    if (!needsSidecar)
        return true;
    if (sidecar.isEmpty()) {
        sidecar = QFileInfo(item.files.first()).absoluteDir().filePath(item.baseName + QStringLiteral(".xmp"));
        if (createdSidecar && !QFileInfo::exists(sidecar))
            *createdSidecar = sidecar;
    }
    return inat::writeSidecarNotes(sidecar, fields, description, error);
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

OrganisePlan OrganiseSession::plan(bool markUnsure) const
{
    OrganisePlan plan;
    QHash<QString, int> folderIndex;   // folded name -> plan.folders index
    for (int i = 0; i < count(); ++i) {
        if (m_rejected.contains(i)) {
            plan.rejects << i;
            continue;
        }
        QString name = runName(i);
        if (name.isEmpty()) {
            ++plan.unnamed;
            continue;
        }
        if (markUnsure && isUnsure(i))
            name = uncertainName(name);
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
            continue;   // (unsure and notes live on named photos)
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
        if (m_unsure.contains(i))
            o.insert(QStringLiteral("unsure"), true);
        if (const auto n = m_notes.constFind(i); n != m_notes.cend()) {
            QJsonObject notes;
            const QList<QPair<const char *, QString>> fields{{"sex", n->sex}, {"lifeStage", n->lifeStage},
                                                             {"alive", n->alive}, {"flowers", n->flowers},
                                                             {"text", n->text}};
            for (const auto &[key, value] : fields) {
                if (!value.isEmpty())
                    notes.insert(QLatin1String(key), value);
            }
            o.insert(QStringLiteral("notes"), notes);
        }
        shots.insert(item.baseName, o);
    }
    // The GPS track chosen for the folder: by name when it's in the folder
    // (so the folder can be moved), else where it is.
    QJsonObject gpx;
    if (m_gpxChosen) {
        const QFileInfo info(m_gpxPath);
        const bool inFolder = !m_gpxPath.isEmpty() && info.absolutePath() == QDir(m_folder).absolutePath();
        gpx = QJsonObject{{QStringLiteral("file"), inFolder ? info.fileName() : m_gpxPath},
                          {QStringLiteral("offsetSeconds"), m_gpxOffsetSeconds}};
    }
    if (shots.isEmpty() && gpx.isEmpty()) {
        QFile::remove(path);   // nothing to keep
        return true;
    }
    QJsonArray order;
    for (const QString &n : m_nameOrder)
        order.append(n);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    QJsonObject root{{QStringLiteral("version"), 1},
                     {QStringLiteral("shots"), shots},
                     {QStringLiteral("recent"), order}};
    if (!gpx.isEmpty())
        root.insert(QStringLiteral("gpx"), gpx);
    file.write(QJsonDocument(root).toJson());
    return file.commit();
}

void OrganiseSession::loadProgress()
{
    const QJsonObject root = applyProgress(0);
    for (const QJsonValue &v : root.value(QStringLiteral("recent")).toArray())
        m_nameOrder << v.toString();
    // The track chosen before; else the folder's own, when it has just one.
    m_gpxPath.clear();
    m_gpxOffsetSeconds = 0;
    m_gpxChosen = false;
    if (root.contains(QStringLiteral("gpx"))) {
        const QJsonObject gpx = root.value(QStringLiteral("gpx")).toObject();
        const QString file = gpx.value(QStringLiteral("file")).toString();
        m_gpxChosen = true;
        m_gpxOffsetSeconds = gpx.value(QStringLiteral("offsetSeconds")).toInt();
        if (!file.isEmpty())
            m_gpxPath = QDir::cleanPath(QDir(m_folder).absoluteFilePath(file));
    } else if (const QStringList found = gpxFilesIn(m_folder); found.size() == 1) {
        m_gpxPath = found.first();
    }
}

void OrganiseSession::setGpx(const QString &path, int offsetSeconds)
{
    m_gpxPath = path.isEmpty() ? QString() : QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    m_gpxOffsetSeconds = m_gpxPath.isEmpty() ? 0 : offsetSeconds;
    m_gpxChosen = true;
    saveProgress();
}

geo::GeotagWriter::Result OrganiseSession::clearLocation(int i, bool intoRaw)
{
    if (i < 0 || i >= count())
        return {};
    Item &item = m_items[i];
    geo::GeotagWriter::Result result =
        geo::GeotagWriter::clearShot(item.files.mid(0, item.kinds.size()), item.baseName, intoRaw);
    // What the files hold now.
    Item reread = item;
    reread.latitude.reset();
    reread.longitude.reset();
    readMetadata(reread);
    item.latitude = reread.latitude;
    item.longitude = reread.longitude;
    if (item.hasGps() && result.ok())
        result.failures << QStringLiteral("%1: a location is still inside its RAW file, which wasn't changed")
                               .arg(item.baseName);
    refreshFileStats(i);
    saveProgress();
    return result;
}

void OrganiseSession::useFoundGpx(const QString &path)
{
    if (m_gpxChosen || path.isEmpty())
        return;
    m_gpxPath = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    m_gpxOffsetSeconds = 0;
}

QStringList OrganiseSession::gpxFilesIn(const QString &folder)
{
    QStringList out;
    if (folder.isEmpty())
        return out;
    const QDir dir(folder);
    for (const QString &name : dir.entryList({QStringLiteral("*.gpx")}, QDir::Files, QDir::Name | QDir::IgnoreCase))
        out << dir.absoluteFilePath(name);
    return out;
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
        if (!name.isEmpty() && o.value(QStringLiteral("unsure")).toBool())
            m_unsure.insert(i);
        if (!name.isEmpty() && o.contains(QStringLiteral("notes"))) {
            const QJsonObject n = o.value(QStringLiteral("notes")).toObject();
            RunNotes notes;
            notes.sex = n.value(QStringLiteral("sex")).toString();
            notes.lifeStage = n.value(QStringLiteral("lifeStage")).toString();
            notes.alive = n.value(QStringLiteral("alive")).toString();
            notes.flowers = n.value(QStringLiteral("flowers")).toString();
            notes.text = n.value(QStringLiteral("text")).toString();
            if (!notes.isEmpty())
                m_notes.insert(i, notes);
        }
    }
    return root;
}

void OrganiseSession::clearProgress()
{
    m_names.clear();
    m_rejected.clear();
    m_best.clear();
    m_namingEnds.clear();
    m_unsure.clear();
    m_notes.clear();
    m_nameOrder.clear();
    QFile::remove(QDir(m_folder).filePath(QLatin1String(kProgressFile)));
    saveProgress();   // the folder's GPS track isn't naming progress: it stays
}

} // namespace pl::organise
