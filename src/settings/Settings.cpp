#include "settings/Settings.h"

#include <QDir>
#include <QStandardPaths>

namespace pl {
namespace {
constexpr auto kWatchedRoots = "library/watchedRoots";
constexpr auto kDatabasePath = "library/databasePath";
constexpr auto kWindowGeometry = "ui/mainWindow/geometry";
constexpr auto kWindowState = "ui/mainWindow/state";
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

} // namespace pl
