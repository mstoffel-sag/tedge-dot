# mbedTLS 3.6 LTS (Apache-2.0), fetched and built once for every C module that needs TLS or
# X.509: the OPC UA connector (open62541 and its PKI code) and the AVEVA PI connector
# (libcurl). Built from the release tarball and linked statically: no system crypto library,
# so the binary keeps its libc-only dependency and the glibc floor of the cross build. The
# release tarball (not the git tag) is used because it carries the generated files and needs
# no submodules.
#
# tdot_fetch_mbedtls() may be called more than once. Afterwards _tdot_mbedtls_inc and
# _tdot_mbedtls_link name the in-tree headers and libraries.
#
# open62541 and curl locate mbedTLS with find_path/find_library, which cannot see libraries
# that are only built later. Their result variables are pre-seeded with the paths the in-tree
# libraries WILL have; each user enforces the build order with add_dependencies on mbedtls,
# mbedx509 and mbedcrypto. Paths, not target names: open62541 exports its targets, and an
# export may not reference a target outside any export set (turning mbedTLS's own export on
# instead would also write to the user's CMake package registry).
include(FetchContent)

set(TDOT_MBEDTLS_URL
    "https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.7/mbedtls-3.6.7.tar.bz2"
    CACHE STRING "mbedTLS 3.6.7 release tarball (a URL or a local path)")
# Read here, at include time: inside the macro, CMAKE_CURRENT_LIST_DIR is the caller's.
set(_tdot_mbedtls_user_config "${CMAKE_CURRENT_LIST_DIR}/mbedtls_user_config.h")

macro(tdot_fetch_mbedtls)
  if(NOT TARGET mbedcrypto)
    # Thread safety (cmake/mbedtls_user_config.h). A directory property, so it reaches the
    # targets of this directory and of every subdirectory added after it: mbedTLS, curl and
    # open62541 are all fetched below this call.
    add_compile_definitions(
      "MBEDTLS_USER_CONFIG_FILE=\"${_tdot_mbedtls_user_config}\"")
    set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "" FORCE)
    set(USE_STATIC_MBEDTLS_LIBRARY ON CACHE BOOL "" FORCE)
    set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(mbedtls
      URL ${TDOT_MBEDTLS_URL}
      URL_HASH SHA256=a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(mbedtls)
    set_target_properties(mbedtls mbedx509 mbedcrypto PROPERTIES
                          ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/_deps/mbedtls-lib")
  endif()
  FetchContent_GetProperties(mbedtls SOURCE_DIR _tdot_mbedtls_src)
  set(_tdot_mbedtls_lib "${CMAKE_BINARY_DIR}/_deps/mbedtls-lib")
  set(_tdot_mbedtls_link mbedx509 mbedcrypto)
  set(_tdot_mbedtls_inc "${_tdot_mbedtls_src}/include")
  # open62541's FindMbedTLS reads MBEDTLS_INCLUDE_DIRS, curl's MBEDTLS_INCLUDE_DIR.
  set(MBEDTLS_INCLUDE_DIRS "${_tdot_mbedtls_inc}" CACHE PATH "" FORCE)
  set(MBEDTLS_INCLUDE_DIR "${_tdot_mbedtls_inc}" CACHE PATH "" FORCE)
  foreach(_lib mbedtls mbedx509 mbedcrypto)
    string(TOUPPER "${_lib}" _var)
    set(${_var}_LIBRARY
        "${_tdot_mbedtls_lib}/${CMAKE_STATIC_LIBRARY_PREFIX}${_lib}${CMAKE_STATIC_LIBRARY_SUFFIX}"
        CACHE FILEPATH "" FORCE)
  endforeach()
endmacro()
