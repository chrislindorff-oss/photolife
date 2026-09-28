#pragma once

#include "db/CatalogueDescriptor.h"

#include <QSettings>
#include <QDateTime>
#include <QString>
#include <QList>
#include <QStringList>

#include <optional>

namespace pl {

// Thin typed wrapper over QSettings (native format, per-user).
class Settings
{
public:
    Settings();

    // --- catalogue profiles -------------------------------------------------
    //
    // PhotoLife keeps fully separate catalogues ("profiles"): exactly one
    // Local catalogue -- the user's own library on their drives -- and any
    // number of Server catalogues, each a local working copy (in its own
    // folder, data/servers/<id>/) of a shared Postgres catalogue it syncs with
    // on demand (see sync::CatalogueSync). One is active at a time; switching
    // restarts PhotoLife into another. Nothing is ever copied between them.
    //
    // The catalogue settings below -- databasePath(), watchedRoots(), the
    // sync ones, sourceSyncedAt() -- are the ACTIVE profile's.

    struct CatalogueProfile
    {
        QString id;                  // "local", or a server's id
        bool server = false;
        QString name;                // "Local catalogue", or the server's display name
        CatalogueDescriptor pg;      // server: its connection
        QString userName;            // server: your name there
        QString libraryRoot;         // server: this machine's photo library folder

        bool isLocal() const { return !server; }
    };
    static constexpr const char *kLocalProfileId = "local";

    QList<CatalogueProfile> profiles() const;   // Local first, then servers by name
    CatalogueProfile activeProfile() const;
    std::optional<CatalogueProfile> profile(const QString &id) const;
    void setActiveProfile(const QString &id);
    // Adds or updates a server profile; returns its id (made from the name
    // for a new one).
    QString saveServerProfile(const CatalogueProfile &profile);
    void removeServerProfile(const QString &id);
    // The catalogue file of any profile (the active one: databasePath()).
    QString profileDatabasePath(const QString &id) const;

    // Folders the user has added as scan roots (the active profile's).
    QStringList watchedRoots() const;
    void setWatchedRoots(const QStringList &roots);

    // Absolute path of the active catalogue file. The Local one defaults to
    // <AppDataLocation>/catalogue.db (override with setDatabasePath(), which
    // only ever moves the Local one); a server's is
    // <AppDataLocation>/servers/<id>/catalogue.db.
    QString databasePath() const;
    void setDatabasePath(const QString &path);

    // What Application opens: always the active profile's SQLite file.
    CatalogueDescriptor catalogueDescriptor() const;
    // Local SQLite: sets the Local catalogue's file. (A Postgres descriptor
    // is only kept for migrating old settings; see migrateProfiles().)
    void setCatalogueDescriptor(const CatalogueDescriptor &descriptor);

    // Whether the active catalogue is a Server one, and its details.
    bool syncEnabled() const;
    CatalogueDescriptor syncServerDescriptor() const;
    QString syncUserName() const;   // defaults to the OS user name
    void setSyncUserName(const QString &name);
    QString libraryRoot() const;
    void setLibraryRoot(const QString &root);

    // Whether this machine holds the active server's edit lock, as last known.
    bool syncEditing() const;
    void setSyncEditing(bool editing);

    // Whether Sync / Finish Editing first shows what will be sent. On by
    // default; the confirmation's "Don't ask again" turns it off.
    bool syncConfirmSend() const;
    void setSyncConfirmSend(bool confirm);

    // One-time: turns settings from before profiles existed (a single
    // catalogue, maybe with a Postgres or sync server) into profiles. The
    // existing catalogue file stays the Local catalogue, untouched. Safe to
    // call every start. True if it changed anything.
    bool migrateProfiles();

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

    // Whether the app should use pl::ThemeVariant::Dark instead of the
    // default Light theme. Set from the View menu's "Dark Mode" toggle.
    bool darkModeEnabled() const;
    void setDarkModeEnabled(bool enabled);

    // Whether the map draws the active reference tree's region outline. Set
    // from the View menu's "Show Tree Locality on Map" toggle; on by default.
    bool showMapLocality() const;
    void setShowMapLocality(bool show);

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

    // When an external data source (e.g. "ala", "apc") was last synced into
    // the active catalogue; invalid if never.
    QDateTime sourceSyncedAt(const QString &source) const;
    void setSourceSyncedAt(const QString &source, const QDateTime &when);

    // Last folder chosen as the destination for "Export Photo Collection…".
    // Empty until the user has run it once.
    QString exportDestFolder() const;
    void setExportDestFolder(const QString &path);

    // Last collection folder chosen for "Update an existing collection" in
    // the same dialog -- the collection itself, not its parent. Empty until used.
    QString exportCollectionFolder() const;
    void setExportCollectionFolder(const QString &path);

private:
    // The active server's id, or empty for the Local catalogue.
    QString activeServerId() const;
    // A per-profile key: Local keeps the original (pre-profile) keys, a
    // server its own group.
    QString profileKey(const QString &serverKey, const QString &localKey) const;

    mutable QSettings m_settings;
};

} // namespace pl
