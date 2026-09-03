cmake_minimum_required(VERSION 3.21)

project(PhotoLife
    VERSION 0.1.0
    DESCRIPTION "Catalogue nature photos against a taxonomic tree"
    HOMEPAGE_URL "https://github.com/chrisl/photolife"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

option(PHOTOLIFE_BUILD_TESTS "Build the PhotoLife test suite" ON)

include(PhotoLifeVersion)

# Qt 6.4 is the floor: the Linux dev box ships 6.4, Windows CI uses 6.5+.
# Keep to APIs available in 6.4.
find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets Sql Network Concurrent)
qt_standard_project_setup()

add_subdirectory(src)

if(PHOTOLIFE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

include(cmake/Packaging.cmake)
