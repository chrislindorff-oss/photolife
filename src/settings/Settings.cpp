#include "settings/Settings.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QObject>
#include <QStandardPaths>

#include <algorithm>

namespace pl {
namespace {
constexpr auto kWatchedRoots = "library/watchedRoots";
constexpr auto kDatabasePath = "library/databasePath";
constexpr auto kCatalogueBackend = "library/backend";   // "sqlite" | "postgres"
constexpr auto kPgHost = "library/pg/host";
constexpr auto kPgPort = "library/pg/port";
constexpr auto kPgDbName = "library/pg/dbName";
constexpr auto kPgUser = "library/pg/user";
constexpr auto kPgPassword = "library/pg/password";   // stored in plain text, like inatApiToken()
constexpr auto kPgSslMode = "library/pg/sslMode";
constexpr auto kSyncUserName = "library/sync/userName";
constexpr auto kLibraryRoot = "library/sync/libraryRoot";
constexpr auto kSyncEditing = "library/sync/editing";
constexpr auto kSyncConfirmSend = "library/sync/confirmSend";
constexpr auto kWindowGeometry = "ui/mainWindow/geometry";
constexpr auto kWindowState = "ui/mainWindow/state";
constexpr auto kCaptionFields = "ui/captureGrid/captionFields";
constexpr auto kDarkMode = "ui/darkMode";
constexpr auto kShowMapLocality = "ui/showMapLocality";
constexpr auto kInatUsername = "inat/username";
constexpr auto kInatApiToken = "inat/apiToken";
constexpr auto kExportDestFolder = "export/destFolder";
constexpr auto kExportCollectionFolder = "export/collectionFolder";
constexpr auto kExportIncludeRaw = "export/includeRaw";
constexpr auto kExportThumbnailsOnly = "export/thumbnailsOnly";
constexpr auto kExportRenameFiles = "export/renameFiles";
constexpr auto kExportFileNamePattern = "export/fileNamePattern";   // JSON
constexpr auto kExportAuthorName = "export/authorName";
constexpr auto kLibraryRenamePattern = "library/renamePattern";   // JSON
constexpr auto kContactSheetOptions = "export/contactSheet/options";   // JSON
constexpr auto kContactSheetFolder = "export/contactSheet/folder";

// Qt's QFile/QDir never expand a shell-style leading "~" -- a path typed or
// hand-edited that way would otherwise be taken completely literally (as a
// real subdirectory named "~"), silently opening/creating a catalogue in
// the wrong place instead of failing loudly.
QString expandHome(const QString &path)
{
    if (path == QStringLiteral("~"))
        return QDir::homePath();
    if (path.startsWith(QStringLiteral("~/")))
        return QDir::homePath() + path.mid(1);
    return path;
}
} // namespace

Settings::Settings()
{
    protectFile();
}

Settings::~Settings()
{
    m_settings.sync();
    protectFile();
}

void Settings::protectFile()
{
#ifndef Q_OS_WIN
    const QString path = m_settings.fileName();
    const QFileDevice::Permissions others = QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup
                                            | QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther;
    if (QFile::exists(path) && (QFile::permissions(path) & others))
        QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
#endif
}

// --- catalogue profiles -------------------------------------------------------

namespace {

constexpr auto kActiveProfile = "catalogues/active";
constexpr auto kProfilesMigrated = "catalogues/migrated";
constexpr auto kServersGroup = "servers";

QString dataDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
}

// "Photo Club (Neon)" -> "photo-club-neon"
QString slug(const QString &name)
{
    QString out;
    for (const QChar c : name.toLower()) {
        if (c.isLetterOrNumber() && c.unicode() < 128)
            out += c;
        else if (!out.endsWith(QLatin1Char('-')))
            out += QLatin1Char('-');
    }
    while (out.endsWith(QLatin1Char('-')))
        out.chop(1);
    while (out.startsWith(QLatin1Char('-')))
        out.remove(0, 1);
    return out.isEmpty() ? QStringLiteral("server") : out;
}

QString serverKey(const QString &id, const QString &key)
{
    return QStringLiteral("%1/%2/%3").arg(QLatin1String(kServersGroup), id, key);
}

} // namespace

QString Settings::activeServerId() const
{
    const QString id = m_settings.value(QLatin1String(kActiveProfile)).toString();
    if (id.isEmpty() || id == QLatin1String(kLocalProfileId))
        return {};
    return m_settings.contains(serverKey(id, QStringLiteral("name"))) ? id : QString();
}

QString Settings::profileKey(const QString &serverKeyName, const QString &localKey) const
{
    const QString id = activeServerId();
    return id.isEmpty() ? localKey : serverKey(id, serverKeyName);
}

std::optional<Settings::CatalogueProfile> Settings::profile(const QString &id) const
{
    if (id.isEmpty() || id == QLatin1String(kLocalProfileId)) {
        CatalogueProfile local;
        local.id = QString::fromLatin1(kLocalProfileId);
        local.name = QObject::tr("Local catalogue");
        return local;
    }
    if (!m_settings.contains(serverKey(id, QStringLiteral("name"))))
        return std::nullopt;
    CatalogueProfile p;
    p.id = id;
    p.server = true;
    auto v = [&](const char *key, const QVariant &fallback = {}) {
        return m_settings.value(serverKey(id, QLatin1String(key)), fallback);
    };
    p.name = v("name").toString();
    p.pg.backend = CatalogueDescriptor::Backend::Postgres;
    p.pg.pgHost = v("pg/host").toString();
    p.pg.pgPort = v("pg/port", 5432).toInt();
    p.pg.pgDbName = v("pg/dbName").toString();
    p.pg.pgUser = v("pg/user").toString();
    p.pg.pgPassword = v("pg/password").toString();
    p.pg.pgSslMode = v("pg/sslMode", QStringLiteral("require")).toString();
    p.userName = v("userName").toString();
    p.libraryRoot = v("libraryRoot").toString();
    p.photoStore = v("photos/store", QString::fromLatin1(kPhotoStoreNone)).toString();
    p.s3.endpoint = v("photos/s3/endpoint").toString();
    p.s3.region = v("photos/s3/region", QStringLiteral("auto")).toString();
    p.s3.bucket = v("photos/s3/bucket").toString();
    p.s3.prefix = v("photos/s3/prefix").toString();
    p.s3.accessKey = v("photos/s3/accessKey").toString();
    p.s3.secretKey = v("photos/s3/secretKey").toString();   // plain text, like pg/password
    p.drive.clientId = v("photos/drive/clientId").toString();
    p.drive.clientSecret = v("photos/drive/clientSecret").toString();
    p.drive.folderId = v("photos/drive/folder").toString();
    p.drive.refreshToken = v("photos/drive/refreshToken").toString();   // plain text too
    p.drive.account = v("photos/drive/account").toString();
    p.photosAllTrees = v("photos/allTrees", true).toBool();
    p.photosWhenDeletedHere = v("photos/whenDeletedHere", QStringLiteral("ask")).toString();
    for (const QString &id : v("photos/trees").toStringList())
        p.photoTrees << id.toInt();
    return p;
}

QList<Settings::CatalogueProfile> Settings::profiles() const
{
    QList<CatalogueProfile> out{*profile(QString::fromLatin1(kLocalProfileId))};
    m_settings.beginGroup(QLatin1String(kServersGroup));
    const QStringList ids = m_settings.childGroups();
    m_settings.endGroup();
    QList<CatalogueProfile> servers;
    for (const QString &id : ids) {
        if (const auto p = profile(id))
            servers << *p;
    }
    std::sort(servers.begin(), servers.end(), [](const CatalogueProfile &a, const CatalogueProfile &b) {
        return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });
    return out + servers;
}

Settings::CatalogueProfile Settings::activeProfile() const
{
    return *profile(activeServerId());
}

void Settings::setActiveProfile(const QString &id)
{
    m_settings.setValue(QLatin1String(kActiveProfile),
                        profile(id) ? id : QString::fromLatin1(kLocalProfileId));
}

QString Settings::saveServerProfile(const CatalogueProfile &p)
{
    QString id = p.id;
    if (id.isEmpty() || id == QLatin1String(kLocalProfileId)) {
        const QString base = slug(p.name);
        id = base;
        for (int n = 2; profile(id) || id == QLatin1String(kLocalProfileId); ++n)
            id = QStringLiteral("%1-%2").arg(base).arg(n);
    }
    auto set = [&](const char *key, const QVariant &value) {
        m_settings.setValue(serverKey(id, QLatin1String(key)), value);
    };
    set("name", p.name.trimmed().isEmpty() ? p.pg.pgDbName : p.name.trimmed());
    set("pg/host", p.pg.pgHost);
    set("pg/port", p.pg.pgPort);
    set("pg/dbName", p.pg.pgDbName);
    set("pg/user", p.pg.pgUser);
    set("pg/password", p.pg.pgPassword);
    set("pg/sslMode", p.pg.pgSslMode);
    set("userName", p.userName.trimmed());
    set("libraryRoot", p.libraryRoot.trimmed());
    set("photos/store", p.photoStore);
    set("photos/s3/endpoint", p.s3.endpoint.trimmed());
    set("photos/s3/region", p.s3.region.trimmed());
    set("photos/s3/bucket", p.s3.bucket.trimmed());
    set("photos/s3/prefix", p.s3.prefix.trimmed());
    set("photos/s3/accessKey", p.s3.accessKey.trimmed());
    set("photos/s3/secretKey", p.s3.secretKey);
    set("photos/drive/clientId", p.drive.clientId.trimmed());
    set("photos/drive/clientSecret", p.drive.clientSecret.trimmed());
    set("photos/drive/folder", libsync::DriveConfig::folderIdFrom(p.drive.folderId));
    set("photos/drive/refreshToken", p.drive.refreshToken);
    set("photos/drive/account", p.drive.account);
    set("photos/allTrees", p.photosAllTrees);
    set("photos/whenDeletedHere", p.photosWhenDeletedHere);
    QStringList trees;
    for (const int id : p.photoTrees)
        trees << QString::number(id);
    set("photos/trees", trees);
    return id;
}

void Settings::removeServerProfile(const QString &id)
{
    if (id.isEmpty() || id == QLatin1String(kLocalProfileId) || !profile(id))
        return;
    if (m_settings.value(QLatin1String(kActiveProfile)).toString() == id)
        setActiveProfile(QString::fromLatin1(kLocalProfileId));
    m_settings.remove(QStringLiteral("%1/%2").arg(QLatin1String(kServersGroup), id));
}

QString Settings::profileDatabasePath(const QString &id) const
{
    if (id.isEmpty() || id == QLatin1String(kLocalProfileId)) {
        const QString configured = m_settings.value(QLatin1String(kDatabasePath)).toString();
        return configured.isEmpty() ? QDir(dataDir()).filePath(QStringLiteral("catalogue.db"))
                                    : expandHome(configured);
    }
    return QDir(dataDir()).filePath(QStringLiteral("servers/%1/catalogue.db").arg(id));
}

bool Settings::migrateProfiles()
{
    if (m_settings.value(QLatin1String(kProfilesMigrated)).toBool())
        return false;
    m_settings.setValue(QLatin1String(kProfilesMigrated), true);

    // Before profiles: one catalogue, and maybe one server (the old direct
    // Postgres mode, or sync mode). The catalogue file stays the Local one,
    // exactly as it is; the server becomes a profile whose working copy
    // starts empty (it's downloaded on first use).
    const QString backend = m_settings.value(QLatin1String(kCatalogueBackend)).toString();
    const QString host = m_settings.value(QLatin1String(kPgHost)).toString();
    QString serverId;
    if (!host.isEmpty()) {
        CatalogueProfile p;
        p.server = true;
        p.pg.backend = CatalogueDescriptor::Backend::Postgres;
        p.pg.pgHost = host;
        p.pg.pgPort = m_settings.value(QLatin1String(kPgPort), 5432).toInt();
        p.pg.pgDbName = m_settings.value(QLatin1String(kPgDbName)).toString();
        p.pg.pgUser = m_settings.value(QLatin1String(kPgUser)).toString();
        p.pg.pgPassword = m_settings.value(QLatin1String(kPgPassword)).toString();
        p.pg.pgSslMode = m_settings.value(QLatin1String(kPgSslMode), QStringLiteral("require")).toString();
        p.name = p.pg.pgDbName.isEmpty() ? host : p.pg.pgDbName;
        p.userName = m_settings.value(QLatin1String(kSyncUserName)).toString();
        p.libraryRoot = m_settings.value(QLatin1String(kLibraryRoot)).toString();
        serverId = saveServerProfile(p);
    }
    const bool serverWasActive = backend == QLatin1String("sync") || backend == QLatin1String("postgres");
    setActiveProfile(serverWasActive && !serverId.isEmpty() ? serverId
                                                            : QString::fromLatin1(kLocalProfileId));
    m_settings.setValue(QLatin1String(kCatalogueBackend), QStringLiteral("sqlite"));
    m_settings.remove(QLatin1String(kSyncEditing));
    return true;
}

// --- the active profile's catalogue settings ----------------------------------

QStringList Settings::watchedRoots() const
{
    return m_settings.value(profileKey(QStringLiteral("watchedRoots"), QLatin1String(kWatchedRoots)))
        .toStringList();
}

void Settings::setWatchedRoots(const QStringList &roots)
{
    m_settings.setValue(profileKey(QStringLiteral("watchedRoots"), QLatin1String(kWatchedRoots)), roots);
}

QString Settings::databasePath() const
{
    return profileDatabasePath(activeServerId());
}

void Settings::setDatabasePath(const QString &path)
{
    m_settings.setValue(QLatin1String(kDatabasePath), path);   // the Local catalogue's
}

CatalogueDescriptor Settings::catalogueDescriptor() const
{
    return CatalogueDescriptor::sqlite(databasePath());
}

void Settings::setCatalogueDescriptor(const CatalogueDescriptor &descriptor)
{
    if (descriptor.backend == CatalogueDescriptor::Backend::Postgres) {
        // Pre-profile form, kept for migrateProfiles() (and old settings).
        m_settings.setValue(QLatin1String(kCatalogueBackend), QStringLiteral("postgres"));
        m_settings.setValue(QLatin1String(kPgHost), descriptor.pgHost);
        m_settings.setValue(QLatin1String(kPgPort), descriptor.pgPort);
        m_settings.setValue(QLatin1String(kPgDbName), descriptor.pgDbName);
        m_settings.setValue(QLatin1String(kPgUser), descriptor.pgUser);
        m_settings.setValue(QLatin1String(kPgPassword), descriptor.pgPassword);
        m_settings.setValue(QLatin1String(kPgSslMode), descriptor.pgSslMode);
    } else {
        setDatabasePath(descriptor.sqlitePath);
    }
}

bool Settings::syncEnabled() const
{
    return !activeServerId().isEmpty();
}

CatalogueDescriptor Settings::syncServerDescriptor() const
{
    return activeProfile().pg;
}

QString Settings::syncUserName() const
{
    QString fallback = qEnvironmentVariable("USER");
    if (fallback.isEmpty())
        fallback = qEnvironmentVariable("USERNAME");
    const QString name = activeProfile().userName;
    return name.isEmpty() ? fallback : name;
}

void Settings::setSyncUserName(const QString &name)
{
    if (const QString id = activeServerId(); !id.isEmpty())
        m_settings.setValue(serverKey(id, QStringLiteral("userName")), name.trimmed());
}

QString Settings::libraryRoot() const
{
    return activeProfile().libraryRoot;
}

void Settings::setLibraryRoot(const QString &root)
{
    if (const QString id = activeServerId(); !id.isEmpty())
        m_settings.setValue(serverKey(id, QStringLiteral("libraryRoot")), root.trimmed());
}

bool Settings::syncEditing() const
{
    const QString id = activeServerId();
    return !id.isEmpty() && m_settings.value(serverKey(id, QStringLiteral("editing")), false).toBool();
}

void Settings::setSyncEditing(bool editing)
{
    if (const QString id = activeServerId(); !id.isEmpty())
        m_settings.setValue(serverKey(id, QStringLiteral("editing")), editing);
}

bool Settings::syncConfirmSend() const
{
    return m_settings.value(QLatin1String(kSyncConfirmSend), true).toBool();
}

void Settings::setSyncConfirmSend(bool confirm)
{
    m_settings.setValue(QLatin1String(kSyncConfirmSend), confirm);
}

void Settings::sync()
{
    m_settings.sync();
    protectFile();
}

QByteArray Settings::mainWindowGeometry() const
{
    return m_settings.value(QLatin1String(kWindowGeometry)).toByteArray();
}

void Settings::setMainWindowGeometry(const QByteArray &geometry)
{
    m_settings.setValue(QLatin1String(kWindowGeometry), geometry);
}

QByteArray Settings::mainWindowState() const
{
    return m_settings.value(QLatin1String(kWindowState)).toByteArray();
}

void Settings::setMainWindowState(const QByteArray &state)
{
    m_settings.setValue(QLatin1String(kWindowState), state);
}

int Settings::captureCaptionFields() const
{
    return m_settings.value(QLatin1String(kCaptionFields), 1).toInt();   // 1 = CaptionName
}

void Settings::setCaptureCaptionFields(int fields)
{
    m_settings.setValue(QLatin1String(kCaptionFields), fields);
}

bool Settings::darkModeEnabled() const
{
    return m_settings.value(QLatin1String(kDarkMode), false).toBool();
}

void Settings::setDarkModeEnabled(bool enabled)
{
    m_settings.setValue(QLatin1String(kDarkMode), enabled);
}

bool Settings::missingShowsInfraspecific() const
{
    return m_settings.value(QStringLiteral("ui/missingShowsInfraspecific"), true).toBool();
}

void Settings::setMissingShowsInfraspecific(bool shown)
{
    m_settings.setValue(QStringLiteral("ui/missingShowsInfraspecific"), shown);
}

bool Settings::missingAsGrid() const
{
    return m_settings.value(QStringLiteral("ui/missingAsGrid"), false).toBool();
}

void Settings::setMissingAsGrid(bool grid)
{
    m_settings.setValue(QStringLiteral("ui/missingAsGrid"), grid);
}

bool Settings::lifeListShowsInfraspecific() const
{
    return m_settings.value(QStringLiteral("ui/lifeListShowsInfraspecific"), false).toBool();
}

void Settings::setLifeListShowsInfraspecific(bool show)
{
    m_settings.setValue(QStringLiteral("ui/lifeListShowsInfraspecific"), show);
}

bool Settings::treeShowsSeen() const
{
    return m_settings.value(QStringLiteral("ui/treeShowsSeen"), false).toBool();
}

void Settings::setTreeShowsSeen(bool show)
{
    m_settings.setValue(QStringLiteral("ui/treeShowsSeen"), show);
}

QString Settings::treeViewMode() const
{
    return m_settings.value(QStringLiteral("ui/treeViewMode"), QStringLiteral("tree")).toString();
}

void Settings::setTreeViewMode(const QString &mode)
{
    m_settings.setValue(QStringLiteral("ui/treeViewMode"), mode);
}

QStringList Settings::lifeListHiddenColumns() const
{
    return m_settings
        .value(QStringLiteral("ui/lifeListHiddenColumns"), QStringList{QStringLiteral("camera"), QStringLiteral("focal")})
        .toStringList();
}

void Settings::setLifeListHiddenColumns(const QStringList &keys)
{
    m_settings.setValue(QStringLiteral("ui/lifeListHiddenColumns"), keys);
}

bool Settings::lifeListThumbnails() const
{
    return m_settings.value(QStringLiteral("ui/lifeListThumbnails"), false).toBool();
}

void Settings::setLifeListThumbnails(bool show)
{
    m_settings.setValue(QStringLiteral("ui/lifeListThumbnails"), show);
}

bool Settings::treePanelShown(const QString &view) const
{
    const bool byDefault = view == QLatin1String("tree") || view == QLatin1String("library");
    return m_settings.value(QStringLiteral("ui/treePanel/") + view, byDefault).toBool();
}

void Settings::setTreePanelShown(const QString &view, bool shown)
{
    m_settings.setValue(QStringLiteral("ui/treePanel/") + view, shown);
}

bool Settings::showMapLocality() const
{
    return m_settings.value(QLatin1String(kShowMapLocality), true).toBool();
}

void Settings::setShowMapLocality(bool show)
{
    m_settings.setValue(QLatin1String(kShowMapLocality), show);
}

QString Settings::inatUsername() const
{
    return m_settings.value(QLatin1String(kInatUsername)).toString();
}

void Settings::setInatUsername(const QString &username)
{
    m_settings.setValue(QLatin1String(kInatUsername), username);
}

QString Settings::inatApiToken() const
{
    return m_settings.value(QLatin1String(kInatApiToken)).toString();
}

QDateTime Settings::sourceSyncedAt(const QString &source) const
{
    const QString key = QStringLiteral("sources/%1/syncedAt").arg(source);
    return m_settings.value(profileKey(key, key)).toDateTime();
}

void Settings::setSourceSyncedAt(const QString &source, const QDateTime &when)
{
    const QString key = QStringLiteral("sources/%1/syncedAt").arg(source);
    m_settings.setValue(profileKey(key, key), when);
}

void Settings::setInatApiToken(const QString &token)
{
    m_settings.setValue(QLatin1String(kInatApiToken), token);
}

QString Settings::exportDestFolder() const
{
    return m_settings.value(QLatin1String(kExportDestFolder)).toString();
}

void Settings::setExportDestFolder(const QString &path)
{
    m_settings.setValue(QLatin1String(kExportDestFolder), path);
}

QString Settings::exportCollectionFolder() const
{
    return m_settings.value(QLatin1String(kExportCollectionFolder)).toString();
}

void Settings::setExportCollectionFolder(const QString &path)
{
    m_settings.setValue(QLatin1String(kExportCollectionFolder), path);
}

bool Settings::exportIncludeRaw() const
{
    return m_settings.value(QLatin1String(kExportIncludeRaw), true).toBool();
}

void Settings::setExportIncludeRaw(bool include)
{
    m_settings.setValue(QLatin1String(kExportIncludeRaw), include);
}

bool Settings::exportThumbnailsOnly() const
{
    return m_settings.value(QLatin1String(kExportThumbnailsOnly), false).toBool();
}

void Settings::setExportThumbnailsOnly(bool thumbnailsOnly)
{
    m_settings.setValue(QLatin1String(kExportThumbnailsOnly), thumbnailsOnly);
}

bool Settings::exportRenameFiles() const
{
    return m_settings.value(QLatin1String(kExportRenameFiles), false).toBool();
}

void Settings::setExportRenameFiles(bool rename)
{
    m_settings.setValue(QLatin1String(kExportRenameFiles), rename);
}

collection::FileNamePattern Settings::exportFileNamePattern() const
{
    const QByteArray json = m_settings.value(QLatin1String(kExportFileNamePattern)).toByteArray();
    return collection::FileNamePattern::fromJson(QJsonDocument::fromJson(json).object());
}

void Settings::setExportFileNamePattern(const collection::FileNamePattern &pattern)
{
    m_settings.setValue(QLatin1String(kExportFileNamePattern),
                        QJsonDocument(pattern.toJson()).toJson(QJsonDocument::Compact));
}

collection::FileNamePattern Settings::libraryRenamePattern() const
{
    const QByteArray json = m_settings.value(QLatin1String(kLibraryRenamePattern)).toByteArray();
    return collection::FileNamePattern::fromJson(QJsonDocument::fromJson(json).object());
}

void Settings::setLibraryRenamePattern(const collection::FileNamePattern &pattern)
{
    m_settings.setValue(QLatin1String(kLibraryRenamePattern),
                        QJsonDocument(pattern.toJson()).toJson(QJsonDocument::Compact));
}

QString Settings::exportAuthorName() const
{
    return m_settings.value(QLatin1String(kExportAuthorName)).toString();
}

void Settings::setExportAuthorName(const QString &name)
{
    m_settings.setValue(QLatin1String(kExportAuthorName), name);
}

collection::ContactSheetOptions Settings::contactSheetOptions() const
{
    const QByteArray json = m_settings.value(QLatin1String(kContactSheetOptions)).toByteArray();
    return collection::ContactSheetOptions::fromJson(QJsonDocument::fromJson(json).object());
}

void Settings::setContactSheetOptions(const collection::ContactSheetOptions &options)
{
    m_settings.setValue(QLatin1String(kContactSheetOptions),
                        QJsonDocument(options.toJson()).toJson(QJsonDocument::Compact));
}

bool Settings::photoOfDayAtStartup() const
{
    return m_settings.value(QStringLiteral("photoOfDay/atStartup"), true).toBool();
}

void Settings::setPhotoOfDayAtStartup(bool on)
{
    m_settings.setValue(QStringLiteral("photoOfDay/atStartup"), on);
}

QString Settings::photoOfDayKey(const char *name) const
{
    const QString key = QStringLiteral("photoOfDay/") + QLatin1String(name);
    return profileKey(key, key);
}

QDate Settings::photoOfDayDate() const
{
    return QDate::fromString(m_settings.value(photoOfDayKey("date")).toString(), Qt::ISODate);
}

int Settings::photoOfDayCaptureId() const
{
    return m_settings.value(photoOfDayKey("captureId"), 0).toInt();
}

QList<int> Settings::photoOfDayShown() const
{
    QList<int> ids;
    const QString value = m_settings.value(photoOfDayKey("shown")).toString();
    for (const QString &id : value.split(QLatin1Char(','), Qt::SkipEmptyParts))
        ids << id.toInt();
    return ids;
}

void Settings::setPhotoOfDay(const QDate &day, int captureId, const QList<int> &shown)
{
    QStringList ids;
    for (int id : shown)
        ids << QString::number(id);
    m_settings.setValue(photoOfDayKey("date"), day.toString(Qt::ISODate));
    m_settings.setValue(photoOfDayKey("captureId"), captureId);
    m_settings.setValue(photoOfDayKey("shown"), ids.join(QLatin1Char(',')));
}

bool Settings::guideShowAtStartup() const
{
    return m_settings.value(QStringLiteral("guide/showAtStartup"), true).toBool();
}

void Settings::setGuideShowAtStartup(bool on)
{
    m_settings.setValue(QStringLiteral("guide/showAtStartup"), on);
}

bool Settings::guideExploredCoverage() const
{
    return m_settings.value(QStringLiteral("guide/exploredCoverage"), false).toBool();
}

void Settings::setGuideExploredCoverage(bool on)
{
    m_settings.setValue(QStringLiteral("guide/exploredCoverage"), on);
}

bool Settings::guideOrganised() const
{
    return m_settings.value(QStringLiteral("guide/organised"), false).toBool();
}

void Settings::setGuideOrganised(bool on)
{
    m_settings.setValue(QStringLiteral("guide/organised"), on);
}

QString Settings::organiseFolder() const
{
    return m_settings.value(QStringLiteral("organise/folder")).toString();
}

void Settings::setOrganiseFolder(const QString &path)
{
    m_settings.setValue(QStringLiteral("organise/folder"), path);
}

bool Settings::organiseShowPlaces() const
{
    return m_settings.value(QStringLiteral("organise/showPlaces"), false).toBool();
}

void Settings::setOrganiseShowPlaces(bool on)
{
    m_settings.setValue(QStringLiteral("organise/showPlaces"), on);
}

int Settings::inatMatchWindow() const
{
    return m_settings.value(QStringLiteral("inat/matchWindow"), 0).toInt();
}

void Settings::setInatMatchWindow(int window)
{
    m_settings.setValue(QStringLiteral("inat/matchWindow"), window);
}

QPointF Settings::geotagLastLocation() const
{
    if (!m_settings.contains(QStringLiteral("geotag/lastLat")))
        return {};
    return QPointF(m_settings.value(QStringLiteral("geotag/lastLon")).toDouble(),
                   m_settings.value(QStringLiteral("geotag/lastLat")).toDouble());
}

QStringList Settings::inatKeptIdentifications() const
{
    return m_settings.value(QStringLiteral("inat/keptIdentifications")).toStringList();
}

void Settings::setInatKeptIdentifications(const QStringList &kept)
{
    m_settings.setValue(QStringLiteral("inat/keptIdentifications"), kept);
}

QString Settings::cardImportFolder() const
{
    return m_settings.value(QStringLiteral("organise/cardImportFolder")).toString();
}

void Settings::setCardImportFolder(const QString &path)
{
    m_settings.setValue(QStringLiteral("organise/cardImportFolder"), path);
}

QString Settings::gpxTracksFolder() const
{
    return m_settings.value(QStringLiteral("organise/gpxTracksFolder")).toString();
}

void Settings::setGpxTracksFolder(const QString &path)
{
    m_settings.setValue(QStringLiteral("organise/gpxTracksFolder"), path);
}

int Settings::tileCacheLimitMb() const
{
    return std::max(0, m_settings.value(QStringLiteral("storage/tileCacheLimitMb"), 500).toInt());
}

void Settings::setTileCacheLimitMb(int megabytes)
{
    m_settings.setValue(QStringLiteral("storage/tileCacheLimitMb"), std::max(0, megabytes));
}

int Settings::geotagLastZoom() const
{
    return m_settings.value(QStringLiteral("geotag/lastZoom"), 13).toInt();
}

void Settings::setGeotagLastView(double latitude, double longitude, int zoom)
{
    m_settings.setValue(QStringLiteral("geotag/lastLat"), latitude);
    m_settings.setValue(QStringLiteral("geotag/lastLon"), longitude);
    m_settings.setValue(QStringLiteral("geotag/lastZoom"), zoom);
}

bool Settings::geotagWriteIntoRaw() const
{
    return m_settings.value(QStringLiteral("geotag/writeIntoRaw"), false).toBool();
}

void Settings::setGeotagWriteIntoRaw(bool on)
{
    m_settings.setValue(QStringLiteral("geotag/writeIntoRaw"), on);
}

bool Settings::mapSatellite() const
{
    return m_settings.value(QStringLiteral("map/satellite"), false).toBool();
}

void Settings::setMapSatellite(bool on)
{
    m_settings.setValue(QStringLiteral("map/satellite"), on);
}

bool Settings::inatIdentifyTreesOnly() const
{
    return m_settings.value(QStringLiteral("inat/identifyTreesOnly"), false).toBool();
}

void Settings::setInatIdentifyTreesOnly(bool on)
{
    m_settings.setValue(QStringLiteral("inat/identifyTreesOnly"), on);
}

bool Settings::inatIdentifyCommonName() const
{
    return m_settings.value(QStringLiteral("inat/identifyCommonName"), true).toBool();
}

void Settings::setInatIdentifyCommonName(bool on)
{
    m_settings.setValue(QStringLiteral("inat/identifyCommonName"), on);
}

int Settings::inatUploadGapMinutes() const
{
    return std::clamp(m_settings.value(QStringLiteral("inat/uploadGapMinutes"), 30).toInt(), 1, 24 * 60);
}

void Settings::setInatUploadGapMinutes(int minutes)
{
    m_settings.setValue(QStringLiteral("inat/uploadGapMinutes"), minutes);
}

bool Settings::inatUploadSplitByDistance() const
{
    return m_settings.value(QStringLiteral("inat/uploadSplitByDistance"), false).toBool();
}

void Settings::setInatUploadSplitByDistance(bool on)
{
    m_settings.setValue(QStringLiteral("inat/uploadSplitByDistance"), on);
}

int Settings::inatUploadDistanceMetres() const
{
    return std::clamp(m_settings.value(QStringLiteral("inat/uploadDistanceMetres"), 500).toInt(), 50, 50000);
}

void Settings::setInatUploadDistanceMetres(int metres)
{
    m_settings.setValue(QStringLiteral("inat/uploadDistanceMetres"), metres);
}

int Settings::inatUploadThumbPx() const
{
    return std::clamp(m_settings.value(QStringLiteral("inat/uploadThumbPx"), 128).toInt(), 64, 256);
}

void Settings::setInatUploadThumbPx(int px)
{
    m_settings.setValue(QStringLiteral("inat/uploadThumbPx"), px);
}

QString Settings::inatUploadGeoprivacy() const
{
    const QString value = m_settings.value(QStringLiteral("inat/uploadGeoprivacy")).toString();
    if (value == QLatin1String("obscured") || value == QLatin1String("private"))
        return value;
    return QStringLiteral("open");
}

void Settings::setInatUploadGeoprivacy(const QString &geoprivacy)
{
    m_settings.setValue(QStringLiteral("inat/uploadGeoprivacy"), geoprivacy);
}

bool Settings::inatRenameWithCommonName() const
{
    return m_settings.value(QStringLiteral("inat/renameWithCommonName"), true).toBool();
}

void Settings::setInatRenameWithCommonName(bool on)
{
    m_settings.setValue(QStringLiteral("inat/renameWithCommonName"), on);
}

bool Settings::organiseRenameOnOrganise() const
{
    return m_settings.value(QStringLiteral("organise/renameOnOrganise"), false).toBool();
}

void Settings::setOrganiseRenameOnOrganise(bool on)
{
    m_settings.setValue(QStringLiteral("organise/renameOnOrganise"), on);
}

QString Settings::contactSheetFolder() const
{
    return m_settings.value(QLatin1String(kContactSheetFolder)).toString();
}

void Settings::setContactSheetFolder(const QString &path)
{
    m_settings.setValue(QLatin1String(kContactSheetFolder), path);
}

} // namespace pl
