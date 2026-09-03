# Derives a descriptive version string from git (falling back to the project
# version) and exposes it as PHOTOLIFE_VERSION_FULL. Re-run at build time is not
# attempted; a reconfigure picks up new tags.

set(PHOTOLIFE_VERSION_FULL "${PROJECT_VERSION}")

find_package(Git QUIET)
if(GIT_FOUND AND EXISTS "${PROJECT_SOURCE_DIR}/.git")
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" describe --tags --always --dirty --match "v*"
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
        OUTPUT_VARIABLE _git_describe
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(_git_describe)
        string(REGEX REPLACE "^v" "" _git_describe "${_git_describe}")
        set(PHOTOLIFE_VERSION_FULL "${_git_describe}")
    endif()
endif()

message(STATUS "PhotoLife version: ${PHOTOLIFE_VERSION_FULL}")
