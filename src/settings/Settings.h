#pragma once

#include "collection/ContactSheetWriter.h"
#include "collection/FileNamePattern.h"
#include "db/CatalogueDescriptor.h"
#include "libsync/DriveConfig.h"
#include "libsync/S3Config.h"

#include <QSettings>
#include <QDateTime>
#include <QPointF>
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

    static constexpr const char *kPhotoStoreNone = "none";
    static constexpr const char *kPhotoStoreS3 = "s3";
    static constexpr const char *kPhotoStoreDrive = "gdrive";

    struct CatalogueProfile
    {
        QString id;                  // "local", or a server's id
        bool server = false;
        QString name;                // "Local catalogue", or the server's display name
        CatalogueDescriptor pg;      // server: its connection
        QString userName;            // server: your name there
        QString libraryRoot;         // server: this machine's photo library folder
        // server: where the shared photos are kept online, if PhotoLife syncs
        // them itself ("none": collaborators share the folder some other way).
        QString photoStore = QString::fromLatin1(kPhotoStoreNone);
        libsync::S3Config s3;        // photoStore == "s3"
        libsync::DriveConfig drive;  // photoStore == "gdrive"
        // Which photos this computer keeps: all, or only those of these
        // reference trees (project ids) -- see libsync::PhotoSelection.
        bool photosAllTrees = true;
        QList<int> photoTrees;
        // Photos deleted on this computer: ask whether to remove them online
        // too, always do, or never (libsync::PhotoSyncService::kDeletedHere*).
        QString photosWhenDeletedHere = QStringLiteral("ask");

        bool syncsPhotos() const { return server && photoStore != QLatin1String(kPhotoStoreNone); }

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
    // Whether the Reference Trees panel is shown in a view ("tree",
    // "library", "review", "inat", "organise"): by default only where the
    // tree is used -- the Reference Tree and All Library Photos views.
    bool treePanelShown(const QString &view) const;
    void setTreePanelShown(const QString &view, bool shown);

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

    // Whether the same dialog's "Include RAW files" box was last ticked.
    // True until the user has changed it.
    bool exportIncludeRaw() const;
    void setExportIncludeRaw(bool include);

    // Whether the same dialog last exported thumbnails rather than the
    // original files. False until the user has changed it.
    bool exportThumbnailsOnly() const;
    void setExportThumbnailsOnly(bool thumbnailsOnly);

    // The same dialog's file-naming choices: whether files were last renamed
    // by a pattern, the pattern itself (kept while renaming is off, so it
    // isn't lost), and the author name used in it. Off and empty until set.
    bool exportRenameFiles() const;
    void setExportRenameFiles(bool rename);
    collection::FileNamePattern exportFileNamePattern() const;
    void setExportFileNamePattern(const collection::FileNamePattern &pattern);
    QString exportAuthorName() const;
    void setExportAuthorName(const QString &name);
    // Rename Files on Drive's "build the whole name from parts" pattern,
    // kept apart from the export's.
    collection::FileNamePattern libraryRenamePattern() const;
    void setLibraryRenamePattern(const collection::FileNamePattern &pattern);

    // The Reference Photos "Export Contact Sheet (PDF)…" dialog's last
    // choices (all but the title and subtitle, which follow the tree), and
    // the folder its last PDF was saved in. Defaults / empty until used.
    collection::ContactSheetOptions contactSheetOptions() const;
    void setContactSheetOptions(const collection::ContactSheetOptions &options);
    QString contactSheetFolder() const;
    void setContactSheetFolder(const QString &path);

    // The download folder last opened in the Organise view.
    QString organiseFolder() const;
    void setOrganiseFolder(const QString &path);
    // Whether Organise shows each photo's place (looked up from its GPS).
    bool organiseShowPlaces() const;
    void setOrganiseShowPlaces(bool on);
    // How near in time an iNaturalist observation must be to the photos it's
    // matched with: 0 minutes, 1 hours, 2 a day either side.
    int inatMatchWindow() const;
    void setInatMatchWindow(int window);

    // Set Location on Map: where the map was last left (a new photo with
    // nothing nearer to go by starts there), and whether a location is also
    // written into RAW files themselves (else only their XMP sidecars).
    QPointF geotagLastLocation() const;   // x = longitude, y = latitude; null if never set
    int geotagLastZoom() const;
    void setGeotagLastView(double latitude, double longitude, int zoom);
    bool geotagWriteIntoRaw() const;
    void setGeotagWriteIntoRaw(bool on);
    // Every map's base layer: aerial photos (Esri) rather than the street map.
    bool mapSatellite() const;
    void setMapSatellite(bool on);
    // Renaming photos from an iNaturalist observation: with its common name
    // (else its scientific name).
    // Identify with iNaturalist: list only suggestions in a reference tree;
    // and, where a suggestion names photos (Organise), with its common name
    // (else its scientific name).
    bool inatIdentifyTreesOnly() const;
    void setInatIdentifyTreesOnly(bool on);
    bool inatIdentifyCommonName() const;
    void setInatIdentifyCommonName(bool on);
    bool inatRenameWithCommonName() const;
    void setInatRenameWithCommonName(bool on);
    // Upload Observations: the most time between two photos of a species that
    // go up as one observation, and the geoprivacy uploads are given.
    int inatUploadGapMinutes() const;
    void setInatUploadGapMinutes(int minutes);
    // ...and, when on, only this close together (photos with a location).
    bool inatUploadSplitByDistance() const;
    void setInatUploadSplitByDistance(bool on);
    int inatUploadDistanceMetres() const;
    void setInatUploadDistanceMetres(int metres);
    // The size of the photos in the upload table.
    int inatUploadThumbPx() const;
    void setInatUploadThumbPx(int px);
    QString inatUploadGeoprivacy() const;   // "open" (the default) | "obscured" | "private"
    void setInatUploadGeoprivacy(const QString &geoprivacy);
    // Whether Organise into Folders also renames the photos.
    bool organiseRenameOnOrganise() const;
    void setOrganiseRenameOnOrganise(bool on);

    // The Getting Started guide: open it at start-up while the basics aren't
    // done; and the steps it can't see in the catalogue.
    bool guideShowAtStartup() const;
    void setGuideShowAtStartup(bool on);
    // Nature Photo of the Day: whether it pops up when PhotoLife opens, and
    // which photo it is today (and which have had their day already) -- the
    // latter per catalogue, whose photos they are.
    bool photoOfDayAtStartup() const;
    void setPhotoOfDayAtStartup(bool on);
    QDate photoOfDayDate() const;
    int photoOfDayCaptureId() const;
    QList<int> photoOfDayShown() const;
    void setPhotoOfDay(const QDate &day, int captureId, const QList<int> &shown);
    bool guideExploredCoverage() const;
    void setGuideExploredCoverage(bool on);
    bool guideOrganised() const;
    void setGuideOrganised(bool on);

private:
    // The active server's id, or empty for the Local catalogue.
    QString activeServerId() const;
    // A per-profile key: Local keeps the original (pre-profile) keys, a
    // server its own group.
    QString photoOfDayKey(const char *name) const;   // per catalogue
    QString profileKey(const QString &serverKey, const QString &localKey) const;

    mutable QSettings m_settings;
};

} // namespace pl
