cmake_minimum_required(VERSION 3.20)
project(file_transfer VERSION 0.1.0 LANGUAGES CXX)
enable_testing()
include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Wpedantic)

add_subdirectory(src)
add_subdirectory(tests)

install(FILES packaging/lan-file-transfer.desktop
    DESTINATION share/applications
)

install(FILES resources/icons/lan-file-transfer.svg
    DESTINATION share/icons/hicolor/scalable/apps
)

set(CPACK_PACKAGE_NAME "lan-file-transfer")
set(CPACK_PACKAGE_VENDOR "lan-file-transfer")
set(CPACK_PACKAGE_CONTACT "dzy")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "LAN file transfer tool")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://example.invalid/lan-file-transfer")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_FILE_NAME
    "${CPACK_PACKAGE_NAME}_${PROJECT_VERSION}_${CMAKE_SYSTEM_PROCESSOR}"
)
set(CPACK_GENERATOR "DEB")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "dzy")
set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)

include(CPack)
