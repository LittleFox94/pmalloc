cmake_minimum_required(VERSION 3.18)

project(pmalloc)

if(NOT ${CMAKE_SYSTEM_NAME} STREQUAL "Linux")
    message(FATAL_ERROR "Only linux is supported")
endif()

add_library(pmalloc pmalloc.c pmalloc.h)
set_target_properties(pmalloc PROPERTIES PUBLIC_HEADER pmalloc.h SOVERSION 0)

if(${BUILD_TESTING})
    include(CTest)
    find_package(GTest)

    add_executable(pmalloc_test pmalloc_test.cpp)
    target_link_libraries(pmalloc_test pmalloc GTest::Main)

    add_test(NAME pmalloc COMMAND pmalloc_test)
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

set(CPACK_DEBIAN_DEV_PACKAGE_NAME libpmalloc-dev)
set(CPACK_DEBIAN_DEV_PACKAGE_SECTION devel)
set(CPACK_DEBIAN_DEV_PACKAGE_GENERATE_SHLIBS ON)
set(CPACK_DEBIAN_DEV_PACKAGE_DEPENDS "libpmalloc")
set(CPACK_DEBIAN_DEV_PACKAGE_SUGGESTS "cmake, build-essentials")

include(GNUInstallDirs)

install(
    TARGETS
        pmalloc
    LIBRARY
        COMPONENT main
        NAMELINK_COMPONENT dev
    PUBLIC_HEADER
        COMPONENT dev
)

include(CPack)
include(CPackComponent)
