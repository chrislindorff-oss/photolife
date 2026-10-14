#include "settings/Settings.h"

#include <QDir>
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

Settings::Settings() = default;

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

QString Settings::exportAuthorName() const
{
    return m_settings.value(QLatin1String(kExportAuthorName)).toString();
}

void Settings::setExportAuthorName(const QString &name)
{
    m_settings.setValue(QLatin1String(kExportAuthorName), name);
}

} // namespace pl
