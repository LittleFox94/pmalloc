cmake_minimum_required(VERSION 3.10)

project(pmalloc)

if(NOT ${CMAKE_SYSTEM_NAME} STREQUAL "Linux")
    message(FATAL_ERROR "Only linux is supported")
endif()

add_library(pmalloc pmalloc.c pmalloc.h)
target_link_libraries(pmalloc m)
set_target_properties(pmalloc PROPERTIES PUBLIC_HEADER pmalloc.h SOVERSION 0)

find_package(GTest)

if(GTEST_FOUND)
    add_executable(pmalloc_test pmalloc_test.cpp)
    target_link_libraries(pmalloc_test pmalloc GTest::Main)

    add_custom_target(test pmalloc_test --gtest_output=xml:junit.xml)
else()
    message(WARNING "Did not find gtest, cannot build tests")
endif()

set(CPACK_PACKAGE_CONTACT "Mara Sophie Grosch <littlefox@lf-net.org>")
set(CPACK_PACKAGE_HOMEPAGE "https://github.com/LittleFox94/pmalloc")
set(CPACK_PAGE_DESCRIPTION "Userspace memory allocator for physical continuous memory")

set(CPACK_GENERATOR        "DEB")
set(CPACK_SOURCE_GENERATOR "TGZ")

set(CPACK_DEBIAN_PACKAGE_CONTROL_STRICT_PERMISSION ON)
set(CPACK_DEB_COMPONENT_INSTALL ON)
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)

set(CPACK_DEBIAN_MAIN_PACKAGE_NAME libpmalloc)
set(CPACK_DEBIAN_MAIN_PACKAGE_SECTION libs)
set(CPACK_DEBIAN_MAIN_PACKAGE_GENERATE_SHLIBS ON)
set(CPACK_DEBIAN_MAIN_DEBUGINFO_PACKAGE ON)

set(CPACK_DEBIAN_DEV_PACKAGE_NAME libpmalloc-dev)
set(CPACK_DEBIAN_DEV_PACKAGE_SECTION devel)
set(CPACK_DEBIAN_DEV_PACKAGE_GENERATE_SHLIBS ON)
set(CPACK_DEBIAN_DEV_PACKAGE_DEPENDS "libpmalloc")
set(CPACK_DEBIAN_DEV_PACKAGE_SUGGESTS "cmake, build-essentials")

set(CMAKE_INSTALL_PREFIX /usr)
include(GNUInstallDirs)

if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.12.0")
    install(
        TARGETS
            pmalloc
        LIBRARY
            COMPONENT main
            DESTINATION ${CMAKE_INSTALL_LIBDIR}
            NAMELINK_COMPONENT dev
        ARCHIVE
            COMPONENT main
            DESTINATION ${CMAKE_INSTALL_LIBDIR}
        PUBLIC_HEADER
            COMPONENT dev
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )
else()
    install(
        TARGETS
            pmalloc
        LIBRARY
            COMPONENT main
            DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE
            COMPONENT main
            DESTINATION ${CMAKE_INSTALL_LIBDIR}
        PUBLIC_HEADER
            COMPONENT dev
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )
endif()

include(CPack)
include(CPackComponent)
