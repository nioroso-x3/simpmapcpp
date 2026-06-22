cmake_minimum_required(VERSION 3.21)
project(simplemap VERSION 0.3.0 LANGUAGES C CXX)

# ----------------------------------------------------------------------------
# Project options
# ----------------------------------------------------------------------------
option(SIMPLEMAP_BUILD_EXAMPLE
    "Build the map_example CLI tool" ${PROJECT_IS_TOP_LEVEL})

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Sensible defaults when this is the top-level project
if(PROJECT_IS_TOP_LEVEL AND NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ----------------------------------------------------------------------------
# Dependencies: OpenCV (required on both platforms)
# ----------------------------------------------------------------------------
#
# Linux: install libopencv-dev
# Android: download the OpenCV-android-sdk and pass
#   -DOpenCV_DIR=/path/to/OpenCV-android-sdk/sdk/native/jni
find_package(OpenCV REQUIRED COMPONENTS core imgcodecs imgproc)

# ----------------------------------------------------------------------------
# Dependencies: SQLite (bundled amalgamation in third_party/sqlite3/)
# ----------------------------------------------------------------------------
if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite3/sqlite3.c)
    message(FATAL_ERROR
        "SQLite amalgamation not found in third_party/sqlite3/. "
        "Download sqlite-amalgamation-*.zip from https://sqlite.org/download.html "
        "and extract sqlite3.c and sqlite3.h into third_party/sqlite3/. "
        "See third_party/sqlite3/README.md.")
endif()

add_library(simplemap_sqlite3 STATIC third_party/sqlite3/sqlite3.c)
# PRIVATE include dir: only sqlite3.c itself needs to find sqlite3.h via this
# target. simplemap's own .cpp files get the path explicitly below. Consumers
# of simplemap never need it because no public header exposes sqlite types.
target_include_directories(simplemap_sqlite3 PRIVATE third_party/sqlite3)
target_compile_definitions(simplemap_sqlite3 PRIVATE
    SQLITE_THREADSAFE=1
    SQLITE_OMIT_LOAD_EXTENSION
    SQLITE_DEFAULT_MEMSTATUS=0
)
if(NOT MSVC)
    # sqlite3.c is a known quantity; silence its warnings
    target_compile_options(simplemap_sqlite3 PRIVATE -w)
endif()

# ----------------------------------------------------------------------------
# Dependencies: cpp-httplib (bundled header-only in third_party/httplib/)
# ----------------------------------------------------------------------------
if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/httplib/httplib.h)
    message(FATAL_ERROR
        "cpp-httplib not found in third_party/httplib/. "
        "Download httplib.h from https://github.com/yhirose/cpp-httplib "
        "into third_party/httplib/. See third_party/httplib/README.md.")
endif()

add_library(simplemap_httplib INTERFACE)
target_include_directories(simplemap_httplib INTERFACE third_party/httplib)
target_compile_definitions(simplemap_httplib INTERFACE CPPHTTPLIB_OPENSSL_SUPPORT)

# ----------------------------------------------------------------------------
# Dependencies: OpenSSL
#
#   Linux: use system OpenSSL (libssl-dev). Fast, well-maintained.
#   Android: ExternalProject_Add downloads and cross-compiles OpenSSL into
#            the build directory. First configure takes a few minutes; cached
#            after that.
# ----------------------------------------------------------------------------
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/OpenSSLDep.cmake)

# ----------------------------------------------------------------------------
# The library
# ----------------------------------------------------------------------------
add_library(simplemap STATIC
    src/MapTileRenderer.cpp
    src/HttplibTileFetcher.cpp
    src/MapLayer.cpp
    src/LayerRenderer.cpp
    src/AsyncTileLoader.cpp
)
add_library(simplemap::simplemap ALIAS simplemap)

target_include_directories(simplemap
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(simplemap
    PUBLIC
        ${OpenCV_LIBS}
    PRIVATE
        simplemap_sqlite3
        simplemap_httplib
        simplemap::openssl_ssl
        simplemap::openssl_crypto
)
target_include_directories(simplemap SYSTEM PRIVATE ${OpenCV_INCLUDE_DIRS})
# simplemap's own .cpp files include <sqlite3.h>; add the path privately so it
# is available to the library build but never propagates to consumers.
target_include_directories(simplemap PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite3)

if(ANDROID)
    # __android_log_print and zlib (httplib uses it for compressed responses)
    target_link_libraries(simplemap PRIVATE log z)
elseif(UNIX)
    find_package(Threads REQUIRED)
    target_link_libraries(simplemap PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
endif()

set_target_properties(simplemap PROPERTIES
    EXPORT_NAME simplemap
)

# ----------------------------------------------------------------------------
# Desktop CLI tools (not built for Android, not built when consumed as a
# subdirectory unless explicitly requested)
# ----------------------------------------------------------------------------
if(SIMPLEMAP_BUILD_EXAMPLE AND NOT ANDROID)
    add_executable(map_example example/main.cpp)
    target_link_libraries(map_example PRIVATE simplemap::simplemap)
    set_target_properties(map_example PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    )

    add_executable(benchmark benchmark/main.cpp)
    target_link_libraries(benchmark PRIVATE simplemap::simplemap)
    set_target_properties(benchmark PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    )
endif()

# ----------------------------------------------------------------------------
# Summary
# ----------------------------------------------------------------------------
if(PROJECT_IS_TOP_LEVEL)
    message(STATUS "")
    message(STATUS "simplemap ${PROJECT_VERSION} configured:")
    message(STATUS "  Build type:       ${CMAKE_BUILD_TYPE}")
    message(STATUS "  Platform:         ${CMAKE_SYSTEM_NAME}")
    if(ANDROID)
        message(STATUS "  Android ABI:      ${ANDROID_ABI}")
        message(STATUS "  Android API:      ${ANDROID_PLATFORM}")
    endif()
    message(STATUS "  OpenCV:           ${OpenCV_VERSION} (${OpenCV_DIR})")
    message(STATUS "  OpenSSL strategy: ${SIMPLEMAP_OPENSSL_STRATEGY}")
    message(STATUS "  Build example:    ${SIMPLEMAP_BUILD_EXAMPLE}")
    message(STATUS "")
endif()
