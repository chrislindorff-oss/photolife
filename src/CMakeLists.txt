# Core library — everything except the entry point, so tests can link it.
qt_add_library(photolife_core STATIC
    app/Application.h        app/Application.cpp
    app/Logging.h           app/Logging.cpp
    app/StorageStats.h      app/StorageStats.cpp
    catalogue/BestShotStore.h catalogue/BestShotStore.cpp
    checklist/ChecklistImporter.h checklist/ChecklistImporter.cpp
    checklist/ChecklistParser.h checklist/ChecklistParser.cpp
    coverage/CoverageCalculator.h coverage/CoverageCalculator.cpp
    db/Database.h           db/Database.cpp
    geo/GeoBox.h            geo/GeoBox.cpp
    geo/LocalityFetcher.h   geo/LocalityFetcher.cpp
    geo/LocalityStore.h     geo/LocalityStore.cpp
    geo/PinClusterer.h      geo/PinClusterer.cpp
    geo/WebMercator.h       geo/WebMercator.cpp
    inat/ExifWriter.h        inat/ExifWriter.cpp
    inat/InatDownloadModel.h inat/InatDownloadModel.cpp
    inat/InatImportService.h inat/InatImportService.cpp
    inat/InatObservationFetcher.h inat/InatObservationFetcher.cpp
    inat/InatPhotoDownloader.h inat/InatPhotoDownloader.cpp
    match/CandidateFinder.h match/CandidateFinder.cpp
    match/MatchEngine.h     match/MatchEngine.cpp
    match/MatchReviewer.h   match/MatchReviewer.cpp
    match/MatchService.h    match/MatchService.cpp
    match/NameParser.h      match/NameParser.cpp
    match/PathClassifier.h  match/PathClassifier.cpp
    match/TaxonResolver.h   match/TaxonResolver.cpp
    match/TextSimilarity.h  match/TextSimilarity.cpp
    model/CaptureListModel.h model/CaptureListModel.cpp
    model/ReferencePhotoModel.h model/ReferencePhotoModel.cpp
    model/ReviewQueueGroupModel.h model/ReviewQueueGroupModel.cpp
    model/ReviewQueueModel.h model/ReviewQueueModel.cpp
    model/TaxonomyTreeModel.h model/TaxonomyTreeModel.cpp
    net/GeocodeClient.h     net/GeocodeClient.cpp
    net/GeocodeParse.h      net/GeocodeParse.cpp
    net/HttpClient.h        net/HttpClient.cpp
    net/PhotoCache.h        net/PhotoCache.cpp
    net/INatClient.h        net/INatClient.cpp
    net/INatParse.h         net/INatParse.cpp
    net/QtNetworkTransport.h net/QtNetworkTransport.cpp
    net/TileCache.h         net/TileCache.cpp
    net/Transport.h
    net/UpdateChecker.h     net/UpdateChecker.cpp
    raw/RawPreview.h        raw/RawPreview.cpp
    scan/CatalogueMaintenance.h scan/CatalogueMaintenance.cpp
    scan/CatalogueWriter.h  scan/CatalogueWriter.cpp
    scan/Exif.h             scan/Exif.cpp
    scan/FileScanner.h      scan/FileScanner.cpp
    scan/FilenameParser.h   scan/FilenameParser.cpp
    scan/LibraryWatcher.h   scan/LibraryWatcher.cpp
    scan/ScanService.h      scan/ScanService.cpp
    scan/ScanTypes.h
    settings/Settings.h     settings/Settings.cpp
    taxonomy/InfraspecificFiller.h taxonomy/InfraspecificFiller.cpp
    taxonomy/ProjectBuilder.h taxonomy/ProjectBuilder.cpp
    taxonomy/ReferencePhotoFetcher.h taxonomy/ReferencePhotoFetcher.cpp
    taxonomy/TaxonomyStore.h taxonomy/TaxonomyStore.cpp
    taxonomy/TaxonomyTypes.h
    thumb/ThumbnailCache.h  thumb/ThumbnailCache.cpp
    ui/AliasEditorDialog.h  ui/AliasEditorDialog.cpp
    ui/CoveragePanel.h      ui/CoveragePanel.cpp
    ui/HelpWindow.h         ui/HelpWindow.cpp
    ui/ImageViewer.h        ui/ImageViewer.cpp
    ui/MainWindow.h         ui/MainWindow.cpp
    ui/MapView.h            ui/MapView.cpp
    ui/NewProjectDialog.h   ui/NewProjectDialog.cpp
    ui/PlaceConfirmDialog.h ui/PlaceConfirmDialog.cpp
    ui/ReferencePhotoDialog.h ui/ReferencePhotoDialog.cpp
    ui/ReviewPane.h         ui/ReviewPane.cpp
    ui/TaxonConfirmDialog.h ui/TaxonConfirmDialog.cpp
    ui/TaxonPickerDialog.h  ui/TaxonPickerDialog.cpp
)

configure_file(Version.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/pl/Version.h" @ONLY)

target_include_directories(photolife_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
)

target_link_libraries(photolife_core PUBLIC
    Qt6::Widgets
    Qt6::Sql
    Qt6::Network
    Qt6::Concurrent
)

# LibRaw is optional: with it, RAW-only captures get an embedded-preview
# thumbnail; without it they show a placeholder.
option(PHOTOLIFE_USE_LIBRAW "Use LibRaw for camera-RAW previews when available" ON)
if(PHOTOLIFE_USE_LIBRAW)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(LibRaw IMPORTED_TARGET libraw)
    endif()
endif()
if(LibRaw_FOUND)
    target_link_libraries(photolife_core PRIVATE PkgConfig::LibRaw)
    target_compile_definitions(photolife_core PRIVATE PHOTOLIFE_HAVE_LIBRAW)
    message(STATUS "LibRaw ${LibRaw_VERSION}: RAW previews enabled")
else()
    message(STATUS "LibRaw not found: RAW-only captures will show a placeholder")
endif()

# exiv2 writes the EXIF metadata (author, GPS, date, taxon) embedded into
# photos downloaded from iNaturalist -- unlike LibRaw, this is a required
# dependency: install libexiv2-dev (Linux), exiv2 (Homebrew), or vcpkg's
# exiv2 port, then reconfigure.
find_package(PkgConfig REQUIRED)
pkg_check_modules(Exiv2 REQUIRED IMPORTED_TARGET exiv2)
target_link_libraries(photolife_core PRIVATE PkgConfig::Exiv2)
target_compile_definitions(photolife_core PRIVATE PHOTOLIFE_HAVE_EXIV2)
message(STATUS "exiv2 ${Exiv2_VERSION}: EXIF writing enabled")

# Schema migrations and the help pages are compiled in so the binary is
# self-contained. There are two parallel sets of migrations, one per SQL
# dialect (see pl::Database) -- keep them in lockstep when adding a new one.
qt_add_resources(photolife_core "photolife_migrations"
    PREFIX "/"
    BASE "${CMAKE_SOURCE_DIR}/resources"
    FILES
        "${CMAKE_SOURCE_DIR}/resources/migrations/sqlite/001_initial.sql"
        "${CMAKE_SOURCE_DIR}/resources/migrations/sqlite/002_taxonomy.sql"
        "${CMAKE_SOURCE_DIR}/resources/migrations/sqlite/003_matching.sql"
        "${CMAKE_SOURCE_DIR}/resources/migrations/sqlite/004_infraspecific.sql"
        "${CMAKE_SOURCE_DIR}/resources/migrations/sqlite/005_geolocation.sql"
        "${CMAKE_SOURCE_DIR}/resources/migrations/sqlite/006_geo_backfill.sql"
        "${CMAKE_SOURCE_DIR}/resources/migrations/sqlite/007_taxon_photos.sql"
        "${CMAKE_SOURCE_DIR}/resources/migrations/sqlite/008_recheck_infraspecific.sql"
        "${CMAKE_SOURCE_DIR}/resources/migrations/sqlite/009_best_shots.sql"
        "${CMAKE_SOURCE_DIR}/resources/migrations/sqlite/010_locality_cache.sql"
        "${CMAKE_SOURCE_DIR}/resources/migrations/sqlite/011_inat_import.sql"
        "${CMAKE_SOURCE_DIR}/resources/migrations/postgres/001_initial.sql"
        "${CMAKE_SOURCE_DIR}/resources/migrations/postgres/002_taxonomy.sql"
        "${CMAKE_SOURCE_DIR}/resources/migrations/postgres/003_matching.sql"
        "${CMAKE_SOURCE_DIR}/resources/migrations/postgres/004_infraspecific.sql"
        "${CMAKE_SOURCE_DIR}/resources/migrations/postgres/005_geolocation.sql"
        "${CMAKE_SOURCE_DIR}/resources/migrations/postgres/006_geo_backfill.sql"
        "${CMAKE_SOURCE_DIR}/resources/migrations/postgres/007_taxon_photos.sql"
        "${CMAKE_SOURCE_DIR}/resources/migrations/postgres/008_recheck_infraspecific.sql"
        "${CMAKE_SOURCE_DIR}/resources/migrations/postgres/009_best_shots.sql"
        "${CMAKE_SOURCE_DIR}/resources/migrations/postgres/010_locality_cache.sql"
        "${CMAKE_SOURCE_DIR}/resources/migrations/postgres/011_inat_import.sql"
)

qt_add_resources(photolife_core "photolife_help"
    PREFIX "/"
    BASE "${CMAKE_SOURCE_DIR}/resources"
    FILES
        "${CMAKE_SOURCE_DIR}/resources/help/help.html"
)

# GUI executable.
qt_add_executable(photolife main.cpp)
target_link_libraries(photolife PRIVATE photolife_core)
set_target_properties(photolife PROPERTIES
    WIN32_EXECUTABLE ON
    OUTPUT_NAME PhotoLife)

if(WIN32)
    string(REPLACE "." "," _ver_commas "${PROJECT_VERSION}")
    target_sources(photolife PRIVATE "${CMAKE_SOURCE_DIR}/packaging/windows/photolife.rc")
    set_source_files_properties("${CMAKE_SOURCE_DIR}/packaging/windows/photolife.rc" PROPERTIES
        COMPILE_DEFINITIONS
        "PHOTOLIFE_VERSION_RC=${_ver_commas},0;PHOTOLIFE_VERSION_STR=\"${PHOTOLIFE_VERSION_FULL}\"")
    target_include_directories(photolife PRIVATE "${CMAKE_SOURCE_DIR}/packaging/windows")
endif()

install(TARGETS photolife
    RUNTIME DESTINATION bin
    BUNDLE DESTINATION .)

if(UNIX AND NOT APPLE)
    install(FILES "${CMAKE_SOURCE_DIR}/packaging/linux/photolife.desktop"
            DESTINATION share/applications)
    install(FILES "${CMAKE_SOURCE_DIR}/packaging/linux/photolife.svg"
            DESTINATION share/icons/hicolor/scalable/apps)
    install(FILES "${CMAKE_SOURCE_DIR}/packaging/linux/photolife.metainfo.xml"
            DESTINATION share/metainfo)
endif()
