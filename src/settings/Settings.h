#pragma once

#include "db/CatalogueDescriptor.h"

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
    // Only meaningful in CatalogueDescriptor::Backend::Sqlite mode -- see
    // catalogueDescriptor() below, which is what Application actually opens.
    QString databasePath() const;
    void setDatabasePath(const QString &path);

    // Which catalogue backend to open (Local SQLite, at databasePath(), or a
    // shared Postgres database) and, for Postgres, the connection details.
    // Takes effect on the next app start -- nothing currently re-points an
    // already-open Database/ScanService/MatchService at a new descriptor.
    CatalogueDescriptor catalogueDescriptor() const;
    void setCatalogueDescriptor(const CatalogueDescriptor &descriptor);

    // Forces any pending writes out to the backing store immediately.
    // QSettings normally buffers writes (flushed periodically or on
    // destruction), which is invisible within one process -- a fresh
    // QSettings in the same process shares the in-memory cache -- but a
    // just-`setCatalogueDescriptor()`'d value can still be unwritten to disk
    // when a *new process* (e.g. the self-relaunch after a catalogue change)
    // starts reading it. Call this before spawning that new process.
    void sync();

    QByteArray mainWindowGeometry() const;
    void setMainWindowGeometry(const QByteArray &geometry);

    QByteArray mainWindowState() const;
    void setMainWindowState(const QByteArray &state);

    // OR of model::CaptureListModel::CaptionField bits — which fields the photo
    // grids show under each thumbnail. Defaults to just the name (bit 1).
    int captureCaptionFields() const;
    void setCaptureCaptionFields(int fields);

    // iNaturalist username the "Download from iNaturalist" tab searches by
    // default (still editable per search). Empty until the user sets one.
    QString inatUsername() const;
    void setInatUsername(const QString &username);

    // A personal API token (from inaturalist.org/users/api_token) sent as a
    // Bearer token so observation lookups run as that authenticated user --
    // reveals true coordinates for geoprivacy-obscured taxa. Optional, empty
    // by default; these tokens expire in ~24h so this is pasted per session,
    // not a permanent login.
    QString inatApiToken() const;
    void setInatApiToken(const QString &token);

private:
    mutable QSettings m_settings;
};

} // namespace pl
