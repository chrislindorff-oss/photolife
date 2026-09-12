#include "settings/Settings.h"

#include <QDir>
#include <QStandardPaths>

namespace pl {
namespace {
constexpr auto kWatchedRoots = "library/watchedRoots";
constexpr auto kDatabasePath = "library/databasePath";
constexpr auto kWindowGeometry = "ui/mainWindow/geometry";
constexpr auto kWindowState = "ui/mainWindow/state";
constexpr auto kCaptionFields = "ui/captureGrid/captionFields";
constexpr auto kInatUsername = "inat/username";
constexpr auto kInatApiToken = "inat/apiToken";
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
        return configured;

    const QString dataDir =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return QDir(dataDir).filePath(QStringLiteral("catalogue.db"));
}

void Settings::setDatabasePath(const QString &path)
{
    m_settings.setValue(QLatin1String(kDatabasePath), path);
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
