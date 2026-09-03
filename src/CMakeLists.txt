# Core library — everything except the entry point, so tests can link it.
qt_add_library(photolife_core STATIC
    app/Application.h        app/Application.cpp
    app/Logging.h           app/Logging.cpp
    checklist/ChecklistImporter.h checklist/ChecklistImporter.cpp
    checklist/ChecklistParser.h checklist/ChecklistParser.cpp
    coverage/CoverageCalculator.h coverage/CoverageCalculator.cpp
    db/Database.h           db/Database.cpp
    match/CandidateFinder.h match/CandidateFinder.cpp
    match/MatchEngine.h     match/MatchEngine.cpp
    match/MatchReviewer.h   match/MatchReviewer.cpp
    match/MatchService.h    match/MatchService.cpp
    match/NameParser.h      match/NameParser.cpp
    match/PathClassifier.h  match/PathClassifier.cpp
    match/TaxonResolver.h   match/TaxonResolver.cpp
    match/TextSimilarity.h  match/TextSimilarity.cpp
    model/CaptureListModel.h model/CaptureListModel.cpp
    model/ReviewQueueModel.h model/ReviewQueueModel.cpp
    model/TaxonomyTreeModel.h model/TaxonomyTreeModel.cpp
    net/HttpClient.h        net/HttpClient.cpp
    net/INatClient.h        net/INatClient.cpp
    net/INatParse.h         net/INatParse.cpp
    net/QtNetworkTransport.h net/QtNetworkTransport.cpp
    net/Transport.h
    net/UpdateChecker.h     net/UpdateChecker.cpp
    raw/RawPreview.h        raw/RawPreview.cpp
    scan/CatalogueWriter.h  scan/CatalogueWriter.cpp
    scan/Exif.h             scan/Exif.cpp
    scan/FileScanner.h      scan/FileScanner.cpp
    scan/FilenameParser.h   scan/FilenameParser.cpp
    scan/LibraryWatcher.h   scan/LibraryWatcher.cpp
    scan/ScanService.h      scan/ScanService.cpp
    scan/ScanTypes.h
    settings/Settings.h     settings/Settings.cpp
    taxonomy/ProjectBuilder.h taxonomy/ProjectBuilder.cpp
    taxonomy/TaxonomyStore.h taxonomy/TaxonomyStore.cpp
    taxonomy/TaxonomyTypes.h
    thumb/ThumbnailCache.h  thumb/ThumbnailCache.cpp
    ui/AliasEditorDialog.h  ui/AliasEditorDialog.cpp
    ui/CoveragePanel.h      ui/CoveragePanel.cpp
    ui/HelpWindow.h         ui/HelpWindow.cpp
    ui/ImageViewer.h        ui/ImageViewer.cpp
    ui/MainWindow.h         ui/MainWindow.cpp
    ui/NewProjectDialog.h   ui/NewProjectDialog.cpp
    ui/ReviewPane.h         ui/ReviewPane.cpp
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

# Schema migrations and the help pages are compiled in so the binary is
# self-contained.
qt_add_resources(photolife_core "photolife_migrations"
    PREFIX "/"
    BASE "${CMAKE_SOURCE_DIR}/resources"
    FILES
        "${CMAKE_SOURCE_DIR}/resources/migrations/001_initial.sql"
        "${CMAKE_SOURCE_DIR}/resources/migrations/002_taxonomy.sql"
        "${CMAKE_SOURCE_DIR}/resources/migrations/003_matching.sql"
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
