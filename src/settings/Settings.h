#pragma once

#include <QSettings>
#include <QString>
#include <QStringList>

namespace pl {

// Thin typed wrapper over QSettings (native format, per-user).
class Settings
{
public:
    Settings();

    // Folders the user has added as scan roots.
    QStringList watchedRoots() const;
    void setWatchedRoots(const QStringList &roots);

    // Absolute path of the catalogue database.
    // Defaults to <AppDataLocation>/catalogue.db; override with setDatabasePath().
    QString databasePath() const;
    void setDatabasePath(const QString &path);

    QByteArray mainWindowGeometry() const;
    void setMainWindowGeometry(const QByteArray &geometry);

    QByteArray mainWindowState() const;
    void setMainWindowState(const QByteArray &state);

private:
    mutable QSettings m_settings;
};

} // namespace pl
