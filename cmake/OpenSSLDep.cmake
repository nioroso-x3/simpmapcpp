# OpenSSLDep.cmake
#
# Provides the imported targets:
#   simplemap::openssl_ssl
#   simplemap::openssl_crypto
#
# On Linux/desktop these alias system OpenSSL found via find_package.
# On Android they wrap a build-time ExternalProject_Add that downloads and
# cross-compiles OpenSSL into the build directory.

if(ANDROID)
    set(SIMPLEMAP_OPENSSL_STRATEGY "ExternalProject (cross-compiled)" PARENT_SCOPE)

    include(ExternalProject)

    set(SIMPLEMAP_OPENSSL_VERSION "3.5.6")
    # Pinned SHA256 of openssl-3.5.6.tar.gz; verify against openssl.org checksums
    # when bumping the version.
    set(SIMPLEMAP_OPENSSL_SHA256
        "deae7c80cba99c4b4f940ecadb3c3338b13cb77418409238e57d7f31f2a3b736")
    set(SIMPLEMAP_OPENSSL_URL
        "https://www.openssl.org/source/openssl-${SIMPLEMAP_OPENSSL_VERSION}.tar.gz")
    set(SIMPLEMAP_OPENSSL_INSTALL ${CMAKE_BINARY_DIR}/_deps/openssl-install)

    # Map Android ABI -> OpenSSL Configure target
    if(ANDROID_ABI STREQUAL "arm64-v8a")
        set(_openssl_target android-arm64)
    elseif(ANDROID_ABI STREQUAL "armeabi-v7a")
        set(_openssl_target android-arm)
    elseif(ANDROID_ABI STREQUAL "x86_64")
        set(_openssl_target android-x86_64)
    elseif(ANDROID_ABI STREQUAL "x86")
        set(_openssl_target android-x86)
    else()
        message(FATAL_ERROR "Unsupported ANDROID_ABI: ${ANDROID_ABI}")
    endif()

    set(_android_api ${ANDROID_NATIVE_API_LEVEL})

    # OpenSSL's Configure needs the NDK toolchain on PATH so it can find
    # clang, llvm-ar, etc.
    set(_toolchain_bin ${CMAKE_ANDROID_NDK}/toolchains/llvm/prebuilt/linux-x86_64/bin)

    ExternalProject_Add(simplemap_openssl_build
        URL ${SIMPLEMAP_OPENSSL_URL}
        URL_HASH SHA256=${SIMPLEMAP_OPENSSL_SHA256}
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE

        CONFIGURE_COMMAND ${CMAKE_COMMAND} -E env
            ANDROID_NDK_ROOT=${CMAKE_ANDROID_NDK}
            "PATH=${_toolchain_bin}:$ENV{PATH}"
            <SOURCE_DIR>/Configure ${_openssl_target}
                --prefix=${SIMPLEMAP_OPENSSL_INSTALL}
                no-shared no-tests no-asm no-legacy no-docs no-apps

        BUILD_COMMAND ${CMAKE_COMMAND} -E env
            ANDROID_NDK_ROOT=${CMAKE_ANDROID_NDK}
            "PATH=${_toolchain_bin}:$ENV{PATH}"
            make -j

        INSTALL_COMMAND ${CMAKE_COMMAND} -E env
            ANDROID_NDK_ROOT=${CMAKE_ANDROID_NDK}
            "PATH=${_toolchain_bin}:$ENV{PATH}"
            make install_sw

        BUILD_IN_SOURCE 1
        BUILD_BYPRODUCTS
            ${SIMPLEMAP_OPENSSL_INSTALL}/lib/libssl.a
            ${SIMPLEMAP_OPENSSL_INSTALL}/lib/libcrypto.a
    )

    # The imported targets' INTERFACE_INCLUDE_DIRECTORIES must point at a
    # directory that exists at configure time, even though it won't be
    # populated until build time.
    file(MAKE_DIRECTORY ${SIMPLEMAP_OPENSSL_INSTALL}/include)

    add_library(simplemap::openssl_crypto STATIC IMPORTED GLOBAL)
    set_target_properties(simplemap::openssl_crypto PROPERTIES
        IMPORTED_LOCATION ${SIMPLEMAP_OPENSSL_INSTALL}/lib/libcrypto.a
        INTERFACE_INCLUDE_DIRECTORIES ${SIMPLEMAP_OPENSSL_INSTALL}/include
    )
    add_dependencies(simplemap::openssl_crypto simplemap_openssl_build)

    add_library(simplemap::openssl_ssl STATIC IMPORTED GLOBAL)
    set_target_properties(simplemap::openssl_ssl PROPERTIES
        IMPORTED_LOCATION ${SIMPLEMAP_OPENSSL_INSTALL}/lib/libssl.a
        INTERFACE_INCLUDE_DIRECTORIES ${SIMPLEMAP_OPENSSL_INSTALL}/include
        INTERFACE_LINK_LIBRARIES simplemap::openssl_crypto
    )
    add_dependencies(simplemap::openssl_ssl simplemap_openssl_build)

else()
    set(SIMPLEMAP_OPENSSL_STRATEGY "system (find_package)")

    find_package(OpenSSL REQUIRED)
    add_library(simplemap::openssl_ssl ALIAS OpenSSL::SSL)
    add_library(simplemap::openssl_crypto ALIAS OpenSSL::Crypto)
endif()
