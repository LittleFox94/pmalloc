cmake_minimum_required(VERSION 3.18)

project(pmalloc)

if(NOT ${CMAKE_SYSTEM_NAME} STREQUAL "Linux")
    message(FATAL_ERROR "Only linux is supported")
endif()

add_library(pmalloc pmalloc.c)

if(${BUILD_TESTING})
    include(CTest)
    find_package(GTest)

    add_executable(pmalloc_test pmalloc_test.cpp)
    target_link_libraries(pmalloc_test pmalloc GTest::Main)

    add_test(NAME pmalloc COMMAND pmalloc_test)
endif()
