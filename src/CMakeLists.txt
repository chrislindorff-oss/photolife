# Core library — everything except the entry point, so tests can link it.
qt_add_library(photolife_core STATIC
    app/Application.h        app/Application.cpp
    app/Logging.h           app/Logging.cpp
    db/Database.h           db/Database.cpp
    match/MatchEngine.h     match/MatchEngine.cpp
    match/MatchService.h    match/MatchService.cpp
    match/NameParser.h      match/NameParser.cpp
    match/PathClassifier.h  match/PathClassifier.cpp
    match/TaxonResolver.h   match/TaxonResolver.cpp
    match/TextSimilarity.h  match/TextSimilarity.cpp
    model/CaptureListModel.h model/CaptureListModel.cpp
    model/TaxonomyTreeModel.h model/TaxonomyTreeModel.cpp
    net/HttpClient.h        net/HttpClient.cpp
    net/INatClient.h        net/INatClient.cpp
    net/INatParse.h         net/INatParse.cpp
    net/QtNetworkTransport.h net/QtNetworkTransport.cpp
    net/Transport.h
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
    ui/MainWindow.h         ui/MainWindow.cpp
    ui/NewProjectDialog.h   ui/NewProjectDialog.cpp
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

# Schema migrations are compiled in so the binary is self-contained.
qt_add_resources(photolife_core "photolife_migrations"
    PREFIX "/"
    BASE "${CMAKE_SOURCE_DIR}/resources"
    FILES
        "${CMAKE_SOURCE_DIR}/resources/migrations/001_initial.sql"
        "${CMAKE_SOURCE_DIR}/resources/migrations/002_taxonomy.sql"
        "${CMAKE_SOURCE_DIR}/resources/migrations/003_matching.sql"
)

# GUI executable.
qt_add_executable(photolife main.cpp)
target_link_libraries(photolife PRIVATE photolife_core)
set_target_properties(photolife PROPERTIES WIN32_EXECUTABLE ON)

install(TARGETS photolife RUNTIME DESTINATION bin)
