# CPack configuration. Produces source and binary tarballs everywhere, plus a
# .deb on Debian-like Linux. AppImage and the Windows installer are built by the
# scripts under packaging/ (and by CI), not by CPack.

set(CPACK_PACKAGE_NAME "PhotoLife")
set(CPACK_PACKAGE_VENDOR "PhotoLife")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_PACKAGE_HOMEPAGE_URL "${PROJECT_HOMEPAGE_URL}")
set(CPACK_PACKAGE_VERSION "${PHOTOLIFE_VERSION_FULL}")
set(CPACK_PACKAGE_VERSION_MAJOR "${PROJECT_VERSION_MAJOR}")
set(CPACK_PACKAGE_VERSION_MINOR "${PROJECT_VERSION_MINOR}")
set(CPACK_PACKAGE_VERSION_PATCH "${PROJECT_VERSION_PATCH}")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "PhotoLife")
set(CPACK_VERBATIM_VARIABLES ON)
set(CPACK_STRIP_FILES ON)

set(CPACK_SOURCE_GENERATOR "TGZ")
set(CPACK_SOURCE_IGNORE_FILES
    "/\\.git/" "/\\.github/" "/build.*/" "/\\.cache/" "\\.user$" "\\.swp$")

if(WIN32)
    set(CPACK_GENERATOR "ZIP")
elseif(APPLE)
    set(CPACK_GENERATOR "TGZ")
else()
    set(CPACK_GENERATOR "TGZ")
    if(EXISTS "/etc/debian_version")
        list(APPEND CPACK_GENERATOR "DEB")
    endif()
endif()

set(CPACK_DEBIAN_PACKAGE_MAINTAINER "PhotoLife <noreply@photolife.local>")
set(CPACK_DEBIAN_PACKAGE_SECTION "graphics")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libqt6sql6-sqlite")

include(CPack)
