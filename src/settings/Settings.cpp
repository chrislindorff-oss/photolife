#include "settings/Settings.h"

#include <QDir>
#include <QStandardPaths>

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
constexpr auto kWindowGeometry = "ui/mainWindow/geometry";
constexpr auto kWindowState = "ui/mainWindow/state";
constexpr auto kCaptionFields = "ui/captureGrid/captionFields";
constexpr auto kDarkMode = "ui/darkMode";
constexpr auto kInatUsername = "inat/username";
constexpr auto kInatApiToken = "inat/apiToken";

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

QStringList Settings::watchedRoots() const
{
    return m_settings.value(QLatin1String(kWatchedRoots)).toStringList();
}

void Settings::setWatchedRoots(const QStringList &roots)
{
    m_settings.setValue(QLatin1String(kWatchedRoots), roots);
}

QString Settings::databasePath() const
{
    const QString configured =
        m_settings.value(QLatin1String(kDatabasePath)).toString();
    if (!configured.isEmpty())
        return expandHome(configured);

    const QString dataDir =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return QDir(dataDir).filePath(QStringLiteral("catalogue.db"));
}

void Settings::setDatabasePath(const QString &path)
{
    m_settings.setValue(QLatin1String(kDatabasePath), path);
}

CatalogueDescriptor Settings::catalogueDescriptor() const
{
    CatalogueDescriptor d;
    // Always populated, regardless of backend -- lets CatalogueSettingsDialog
    // prefill the SQLite file field with the real configured path even when
    // switching back from Postgres, instead of showing it blank.
    d.sqlitePath = databasePath();

    const QString backend = m_settings.value(QLatin1String(kCatalogueBackend)).toString();
    d.backend = backend == QStringLiteral("postgres") ? CatalogueDescriptor::Backend::Postgres
                                                       : CatalogueDescriptor::Backend::Sqlite;

    // Also always populated, regardless of backend -- mirrors sqlitePath
    // above, so CatalogueSettingsDialog can prefill the Postgres tab with
    // the last-used connection even after switching back to Local SQLite,
    // instead of showing it blank. setCatalogueDescriptor() never writes
    // these keys when saving as SQLite, so the last Postgres values saved
    // stay put until the user overwrites them by saving as Postgres again.
    d.pgHost = m_settings.value(QLatin1String(kPgHost)).toString();
    d.pgPort = m_settings.value(QLatin1String(kPgPort), 5432).toInt();
    d.pgDbName = m_settings.value(QLatin1String(kPgDbName)).toString();
    d.pgUser = m_settings.value(QLatin1String(kPgUser)).toString();
    d.pgPassword = m_settings.value(QLatin1String(kPgPassword)).toString();
    d.pgSslMode = m_settings.value(QLatin1String(kPgSslMode), QStringLiteral("prefer")).toString();
    return d;
}

void Settings::setCatalogueDescriptor(const CatalogueDescriptor &descriptor)
{
    if (descriptor.backend == CatalogueDescriptor::Backend::Postgres) {
        m_settings.setValue(QLatin1String(kCatalogueBackend), QStringLiteral("postgres"));
        m_settings.setValue(QLatin1String(kPgHost), descriptor.pgHost);
        m_settings.setValue(QLatin1String(kPgPort), descriptor.pgPort);
        m_settings.setValue(QLatin1String(kPgDbName), descriptor.pgDbName);
        m_settings.setValue(QLatin1String(kPgUser), descriptor.pgUser);
        m_settings.setValue(QLatin1String(kPgPassword), descriptor.pgPassword);
        m_settings.setValue(QLatin1String(kPgSslMode), descriptor.pgSslMode);
    } else {
        m_settings.setValue(QLatin1String(kCatalogueBackend), QStringLiteral("sqlite"));
        setDatabasePath(descriptor.sqlitePath);
    }
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

void Settings::setInatApiToken(const QString &token)
{
    m_settings.setValue(QLatin1String(kInatApiToken), token);
}

} // namespace pl
