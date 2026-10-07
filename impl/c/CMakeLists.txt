cmake_minimum_required(VERSION 3.16)
# Both implementations publish this as the `version` of their capability descriptor, because
# they implement the same contract revision -- the e2e suites rely on that to tell them apart
# by LINKAGE rather than by version (connectors/_shared/stack.resource).
#
# Keeping it in step with impl/rust/Cargo.toml is manual: nothing checks it. The conformance
# suite compares protocol/modes/datatypes/verbs/features/subscribe, not the version string.
project(tedge-dot VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE MinSizeRel) # the point of this build: small binaries
endif()

# Protocol feature flags (mirror the Rust cargo features). The SocketCAN
# connectors are Linux-only, like their Rust counterparts.
option(TDOT_MODBUS "Build the Modbus connector (libmodbus, LGPL-2.1+)" ON)
option(TDOT_OPCUA "Build the OPC UA connector (open62541, MPL-2.0)" ON)
option(TDOT_PROFIBUS "Build the PROFIBUS-DP connector (built-in DP master)" ON)
option(TDOT_SNMP "Build the SNMP trap receiver (POSIX sockets, built-in decoder)" ON)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  option(TDOT_CANBUS "Build the CAN bus connector (SocketCAN + DBC)" ON)
  option(TDOT_CANOPEN "Build the CANopen connector (SDO over SocketCAN)" ON)
else()
  set(TDOT_CANBUS OFF)
  set(TDOT_CANOPEN OFF)
endif()

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(MOSQUITTO REQUIRED IMPORTED_TARGET libmosquitto)
pkg_check_modules(CJSON REQUIRED IMPORTED_TARGET libcjson)

# --- vendored tomlc99 (MIT) --------------------------------------------------
add_library(tomlc99 STATIC third_party/tomlc99/toml.c)
target_include_directories(tomlc99 PUBLIC third_party/tomlc99)

# --- SDK ----------------------------------------------------------------------
add_library(tedge_dot_sdk STATIC
  sdk/src/model.c
  sdk/src/decode.c
  sdk/src/config.c
  sdk/src/descriptor.c
  sdk/src/envelope.c
  sdk/src/report.c
  sdk/src/map.c
  sdk/src/runtime.c
  sdk/src/factory.c
)
target_include_directories(tedge_dot_sdk PUBLIC sdk/include)
# The version every module reports in its capability descriptor (contract §7).
target_compile_definitions(tedge_dot_sdk PUBLIC TDOT_VERSION="${PROJECT_VERSION}")
# The release version `tedge-dot --version` reports. The release build stamps it from the tag
# (cross/build.sh, VERSION); it is independent of the contract version above.
set(TDOT_BUILD_VERSION "${PROJECT_VERSION}" CACHE STRING "Version reported by --version")
target_compile_definitions(tedge_dot_sdk PUBLIC TDOT_BUILD_VERSION="${TDOT_BUILD_VERSION}")
target_link_libraries(tedge_dot_sdk PUBLIC tomlc99 PkgConfig::CJSON
                      PkgConfig::MOSQUITTO m Threads::Threads)

# --- connectors ----------------------------------------------------------------
if(TDOT_MODBUS)
  pkg_check_modules(MODBUS REQUIRED IMPORTED_TARGET libmodbus)
  target_sources(tedge_dot_sdk PRIVATE connectors/modbus/connector_modbus.c)
  target_link_libraries(tedge_dot_sdk PUBLIC PkgConfig::MODBUS)
  target_compile_definitions(tedge_dot_sdk PUBLIC TDOT_FEATURE_MODBUS)
endif()

if(TDOT_OPCUA)
  # Prefer an installed open62541; fall back to building v1.5.5 from source
  # (client-only, reduced feature set) so CI needs no distro package.
  #
  # Cross builds must skip the lookup entirely: find_package() searches the
  # *host* and would hand a host library to the target linker. The cross
  # toolchain (cross/toolchain.cmake) also sets CMAKE_FIND_ROOT_PATH_MODE_PACKAGE,
  # but this option makes the intent explicit and independent of that.
  option(TDOT_OPCUA_VENDORED
         "Always build open62541 from source, never look for an installed one" OFF)
  if(NOT TDOT_OPCUA_VENDORED)
    find_package(open62541 QUIET)
  endif()
  # Secured sessions (policies other than None, certificates, X.509 user identities) need an
  # open62541 built with mbedTLS: the connector's own PKI code (connectors/opcua/ua_pki.c)
  # uses mbedTLS directly, and one crypto library is enough. Distro and Homebrew packages are
  # often built without encryption (or with OpenSSL), so an installed library is checked and,
  # unless it uses mbedTLS and the mbedTLS development files are there, ignored in favour of
  # the source build.
  set(_tdot_ua_lib open62541::open62541)
  if(open62541_FOUND)
    get_target_property(_tdot_ua_inc open62541::open62541 INTERFACE_INCLUDE_DIRECTORIES)
    set(_tdot_ua_crypto "")
    foreach(_dir ${_tdot_ua_inc})
      if(EXISTS "${_dir}/open62541/config.h")
        file(STRINGS "${_dir}/open62541/config.h" _tdot_ua_crypto
             REGEX "^#define UA_ENABLE_ENCRYPTION_MBEDTLS")
        break()
      endif()
    endforeach()
    if(_tdot_ua_crypto)
      find_path(TDOT_MBEDTLS_INCLUDE mbedtls/x509_crt.h)
      find_library(TDOT_MBEDX509 mbedx509)
      find_library(TDOT_MBEDCRYPTO mbedcrypto)
    endif()
    if(NOT _tdot_ua_crypto OR NOT TDOT_MBEDTLS_INCLUDE OR NOT TDOT_MBEDX509
       OR NOT TDOT_MBEDCRYPTO)
      message(STATUS "The installed open62541 (${open62541_DIR}) is not built with mbedTLS "
                     "encryption (or the mbedTLS development files are missing), which secured "
                     "OPC UA sessions need: building v1.5.5 from source.")
      set(open62541_FOUND FALSE)
    else()
      set(_tdot_mbedtls_link ${TDOT_MBEDX509} ${TDOT_MBEDCRYPTO})
      set(_tdot_mbedtls_inc ${TDOT_MBEDTLS_INCLUDE})
    endif()
  endif()
  if(NOT open62541_FOUND)
    # The rejected package already defined the imported open62541::open62541, which shadows
    # the source build's alias of the same name: link the source build's own target.
    set(_tdot_ua_lib open62541)
    include(FetchContent)
    # mbedTLS 3.6 LTS (Apache-2.0), built from the release tarball and linked statically:
    # no system crypto library, so the binary keeps its libc-only dependency and the glibc
    # floor of the cross build. The release tarball (not the git tag) is used because it
    # carries the generated files and needs no submodules.
    set(TDOT_MBEDTLS_URL
        "https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.7/mbedtls-3.6.7.tar.bz2"
        CACHE STRING "mbedTLS 3.6.7 release tarball (a URL or a local path)")
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
    # open62541 locates mbedTLS with find_path/find_library (tools/cmake/FindMbedTLS.cmake),
    # which cannot see libraries that are only built later. Its result variables are
    # pre-seeded with the paths the in-tree libraries WILL have, and the build order is
    # enforced below. Paths, not target names: open62541 exports its targets, and an export
    # may not reference a target outside any export set (turning mbedTLS's own export on
    # instead would also write to the user's CMake package registry).
    FetchContent_GetProperties(mbedtls SOURCE_DIR _tdot_mbedtls_src)
    set(_tdot_mbedtls_lib "${CMAKE_BINARY_DIR}/_deps/mbedtls-lib")
    set_target_properties(mbedtls mbedx509 mbedcrypto PROPERTIES
                          ARCHIVE_OUTPUT_DIRECTORY "${_tdot_mbedtls_lib}")
    set(MBEDTLS_INCLUDE_DIRS "${_tdot_mbedtls_src}/include" CACHE PATH "" FORCE)
    set(_tdot_mbedtls_link mbedx509 mbedcrypto)
    set(_tdot_mbedtls_inc "${_tdot_mbedtls_src}/include")
    foreach(_lib mbedtls mbedx509 mbedcrypto)
      string(TOUPPER "${_lib}" _var)
      set(${_var}_LIBRARY
          "${_tdot_mbedtls_lib}/${CMAKE_STATIC_LIBRARY_PREFIX}${_lib}${CMAKE_STATIC_LIBRARY_SUFFIX}"
          CACHE FILEPATH "" FORCE)
    endforeach()

    set(UA_ENABLE_SUBSCRIPTIONS ON CACHE BOOL "" FORCE)
    set(UA_ENABLE_METHODCALLS OFF CACHE BOOL "" FORCE)
    # PubSub (on by default) needs method calls; its server code only links in optimised
    # builds, where nothing pulls it in. The connector is a client and needs neither.
    set(UA_ENABLE_PUBSUB OFF CACHE BOOL "" FORCE)
    set(UA_ENABLE_NODEMANAGEMENT OFF CACHE BOOL "" FORCE)
    set(UA_ENABLE_AMALGAMATION OFF CACHE BOOL "" FORCE)
    set(UA_ENABLE_ENCRYPTION MBEDTLS CACHE STRING "" FORCE)
    # open62541 compiles itself with ASan/UBSan in Debug builds, which our targets (built
    # without the sanitizer runtime) then fail to link against.
    set(UA_ENABLE_DEBUG_SANITIZER OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(open62541
      GIT_REPOSITORY https://github.com/open62541/open62541.git
      GIT_TAG v1.5.5
      GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(open62541)
    # open62541 builds itself with -Werror, and clang (which is what `zig cc`
    # is) rejects the __DATE__/__TIME__ expansion in ua_config_default.c that
    # gcc lets through. Disable just that warning, per target.
    #
    # Do NOT do this by saving/restoring CMAKE_C_FLAGS around the call:
    # open62541 appends its own required flags to that variable and caches it,
    # so restoring the old value silently drops them and the build fails to
    # link (undefined setMethodNode_callback).
    foreach(_tdot_ua_target open62541 open62541-object open62541-plugins)
      if(TARGET ${_tdot_ua_target})
        target_compile_options(${_tdot_ua_target} PRIVATE -Wno-date-time)
        # Basic128Rsa15 and Basic256 are compiled into the library but left out of the
        # default policy set behind this macro, which open62541 exposes as no CMake option
        # (it appears only in plugins/ua_config_default.c). Without it the C client cannot
        # offer them at all and a device configured for one fails with BadInternalError,
        # while the Rust build connects -- the two would disagree about which servers are
        # reachable. They stay refused unless a device sets `allow_deprecated_security`;
        # that opt-in, not the absence of the code, is what keeps them out of use.
        target_compile_definitions(${_tdot_ua_target} PRIVATE UA_INCLUDE_INSECURE_POLICIES)
        add_dependencies(${_tdot_ua_target} mbedtls mbedx509 mbedcrypto)
      endif()
    endforeach()
  endif()
  target_sources(tedge_dot_sdk PRIVATE connectors/opcua/connector_opcua.c
                                       connectors/opcua/ua_pki.c
                                       connectors/opcua/pki_cli.c)
  target_include_directories(tedge_dot_sdk PRIVATE ${_tdot_mbedtls_inc})
  # open62541 first: it needs mbedTLS itself, and static archives resolve left to right.
  target_link_libraries(tedge_dot_sdk PUBLIC ${_tdot_ua_lib} ${_tdot_mbedtls_link})
  target_compile_definitions(tedge_dot_sdk PUBLIC TDOT_FEATURE_OPCUA)
endif()

if(TDOT_CANBUS)
  target_sources(tedge_dot_sdk PRIVATE connectors/canbus/connector_canbus.c
                                       connectors/canbus/dbc.c)
  target_compile_definitions(tedge_dot_sdk PUBLIC TDOT_FEATURE_CANBUS)
endif()

if(TDOT_CANOPEN)
  target_sources(tedge_dot_sdk PRIVATE connectors/canopen/connector_canopen.c)
  target_compile_definitions(tedge_dot_sdk PUBLIC TDOT_FEATURE_CANOPEN)
endif()

if(TDOT_PROFIBUS)
  target_sources(tedge_dot_sdk PRIVATE
                 connectors/profibus/connector_profibus.c)
  target_compile_definitions(tedge_dot_sdk PUBLIC TDOT_FEATURE_PROFIBUS)
endif()

if(TDOT_SNMP)
  # net-snmp's libnetsnmp (BSD-style), built from the pinned release tarball as a minimal
  # static library: no agent, applications, MIBs or system OpenSSL -- the `internal` crypto
  # copy gives USM MD5/SHA-1 authentication and DES/AES-128 privacy, and the binary gains
  # no runtime dependency beyond libc. Same recipe for native, zig cross (cross/build.sh
  # sets TDOT_NETSNMP_HOST) and the e2e image (connectors/_shared/Dockerfile.connector-c).
  #
  # third_party/net-snmp/tedge-dot.patch makes the decoder as strict as the shared golden
  # vectors require; the file lists each change and why.
  include(ExternalProject)
  include(ProcessorCount)
  ProcessorCount(TDOT_NPROC)
  if(TDOT_NPROC EQUAL 0)
    set(TDOT_NPROC 2)
  endif()
  set(TDOT_NETSNMP_URL
      "https://sourceforge.net/projects/net-snmp/files/net-snmp/5.9.4/net-snmp-5.9.4.tar.gz/download"
      CACHE STRING "net-snmp 5.9.4 release tarball (a URL or a local path)")
  set(TDOT_NETSNMP_SHA256 8b4de01391e74e3c7014beb43961a2d6d6fa03acc34280b9585f4930745b0544)
  set(TDOT_NETSNMP_HOST "" CACHE STRING
      "configure --host triplet for a cross build, e.g. aarch64-linux-gnu (empty: native)")

  set(_tdot_ns_dir ${CMAKE_BINARY_DIR}/_deps/net-snmp)
  string(TOUPPER "${CMAKE_BUILD_TYPE}" _tdot_bt)
  # OPENSSL_NO_ASM: the internal MD5/SHA-1 copy's x86 inline-asm ROTATE feeds a 64-bit
  # operand (from `L`-suffixed constants) to a 32-bit register, which gcc accepts but
  # clang (zig cc) rejects; the portable C rotate compiles to the same `rol` anyway.
  set(_tdot_ns_env CC=${CMAKE_C_COMPILER}
                   "CFLAGS=${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_${_tdot_bt}} -DOPENSSL_NO_ASM")
  set(_tdot_ns_args
      --with-defaults --disable-agent --disable-applications --disable-manuals
      --disable-scripts --disable-mibs --disable-mib-loading --with-mib-modules=
      --without-perl-modules --disable-embedded-perl --with-openssl=internal
      --without-pcre --without-rpm --disable-shared --enable-static
      "--with-transports=UDP UDPIPv6" --with-security-modules=usm)
  if(TDOT_NETSNMP_HOST)
    # libtool insists on a real `ld`, so the target binutils come along with zig cc.
    list(APPEND _tdot_ns_args --host=${TDOT_NETSNMP_HOST} --with-endianness=little)
    list(APPEND _tdot_ns_env AR=${TDOT_NETSNMP_HOST}-ar RANLIB=${TDOT_NETSNMP_HOST}-ranlib
                             LD=${TDOT_NETSNMP_HOST}-ld NM=${TDOT_NETSNMP_HOST}-nm)
  endif()

  ExternalProject_Add(netsnmp_build
    URL ${TDOT_NETSNMP_URL}
    URL_HASH SHA256=${TDOT_NETSNMP_SHA256}
    DOWNLOAD_NAME net-snmp-5.9.4.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_DIR ${_tdot_ns_dir}
    BUILD_IN_SOURCE TRUE
    PATCH_COMMAND patch -p1 -i ${CMAKE_CURRENT_SOURCE_DIR}/third_party/net-snmp/tedge-dot.patch
    CONFIGURE_COMMAND ${CMAKE_COMMAND} -E env ${_tdot_ns_env} ./configure ${_tdot_ns_args}
    # Only the library: the rest of the tree (agent libraries included) is not needed.
    BUILD_COMMAND make -C snmplib -j${TDOT_NPROC}
    INSTALL_COMMAND ""
    BUILD_BYPRODUCTS ${_tdot_ns_dir}/snmplib/.libs/libnetsnmp.a
    LOG_CONFIGURE TRUE
    LOG_BUILD TRUE
    LOG_OUTPUT_ON_FAILURE TRUE)

  # An imported target's include directory must exist when CMake generates.
  file(MAKE_DIRECTORY ${_tdot_ns_dir}/include)
  add_library(tdot_netsnmp STATIC IMPORTED GLOBAL)
  set_target_properties(tdot_netsnmp PROPERTIES
    IMPORTED_LOCATION ${_tdot_ns_dir}/snmplib/.libs/libnetsnmp.a
    INTERFACE_INCLUDE_DIRECTORIES ${_tdot_ns_dir}/include)
  add_dependencies(tdot_netsnmp netsnmp_build)

  target_sources(tedge_dot_sdk PRIVATE connectors/snmp/connector_snmp.c
                                       connectors/snmp/snmp_netsnmp.c
                                       connectors/snmp/snmp_value.c)
  target_link_libraries(tedge_dot_sdk PUBLIC tdot_netsnmp)
  add_dependencies(tedge_dot_sdk netsnmp_build)
  target_compile_definitions(tedge_dot_sdk PUBLIC TDOT_FEATURE_SNMP)
endif()

# --- binary ---------------------------------------------------------------------
add_executable(tedge-dot src/main.c)
target_link_libraries(tedge-dot PRIVATE tedge_dot_sdk)

# --- tests -----------------------------------------------------------------------
enable_testing()
add_executable(tedge-dot-golden tests/golden.c)
target_link_libraries(tedge-dot-golden PRIVATE tedge_dot_sdk)
add_test(NAME golden-vectors
         COMMAND tedge-dot-golden
                 ${CMAKE_CURRENT_SOURCE_DIR}/../rust/crates/sdk/conformance/vectors.json)

# The reporting policy (contract §5.3): the shared vectors the Rust SDK's report.rs runs too.
add_executable(tedge-dot-report tests/report.c)
target_link_libraries(tedge-dot-report PRIVATE tedge_dot_sdk)
add_test(NAME report-vectors
         COMMAND tedge-dot-report
                 ${CMAKE_CURRENT_SOURCE_DIR}/../../doc/contract/test-vectors/report/vectors.json)

# Value mapping (contract §4.3): the shared vectors the Rust SDK's map.rs runs too.
add_executable(tedge-dot-map tests/map.c)
target_link_libraries(tedge-dot-map PRIVATE tedge_dot_sdk)
add_test(NAME map-vectors
         COMMAND tedge-dot-map
                 ${CMAKE_CURRENT_SOURCE_DIR}/../../doc/contract/test-vectors/map/vectors.json)

# Device parameters / `describe`: the same assertions as the Rust SDK's
# descriptor tests (impl/rust/crates/sdk/src/descriptor.rs), over the same fixture config.
add_executable(tedge-dot-describe tests/describe.c)
target_link_libraries(tedge-dot-describe PRIVATE tedge_dot_sdk)
add_test(NAME describe COMMAND tedge-dot-describe)

# Config-loader semantics the runtime relies on: the liveness bounds (§8.1) and the
# per-point sampling hint that keeps push delivery off the polling schedule.
add_executable(tedge-dot-config tests/config.c)
target_link_libraries(tedge-dot-config PRIVATE tedge_dot_sdk)
add_test(NAME config COMMAND tedge-dot-config)

# The write direction of the per-point transform (contract §4.2): the inverse math and the
# one write path every command and the CLI take.
add_executable(tedge-dot-write tests/write.c)
target_link_libraries(tedge-dot-write PRIVATE tedge_dot_sdk)
add_test(NAME write COMMAND tedge-dot-write)

# OPC UA: the PKI directory and certificate trust over the PKI vectors the Rust build
# validates too (connectors/opcua/conformance/tools/genpki.py; needs Python + cryptography).
if(TDOT_OPCUA)
  add_executable(tedge-dot-opcua-sampling tests/opcua_sampling.c)
  target_include_directories(tedge-dot-opcua-sampling PRIVATE connectors/opcua)
  target_link_libraries(tedge-dot-opcua-sampling PRIVATE tedge_dot_sdk)
  add_test(NAME opcua-sampling COMMAND tedge-dot-opcua-sampling)

  add_executable(tedge-dot-opcua-pki tests/opcua_pki.c)
  target_include_directories(tedge-dot-opcua-pki PRIVATE connectors/opcua)
  target_link_libraries(tedge-dot-opcua-pki PRIVATE tedge_dot_sdk)
  add_test(NAME opcua-pki-vectors
           COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/opcua_pki.sh
                   $<TARGET_FILE:tedge-dot-opcua-pki> ${CMAKE_CURRENT_SOURCE_DIR}/../..)
  # `tedge-dot pki`, the scenarios of impl/rust/tests/pki_cli.rs.
  add_test(NAME opcua-pki-cli
           COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/opcua_pki_cli.sh
                   $<TARGET_FILE:tedge-dot> ${CMAKE_CURRENT_SOURCE_DIR}/../..)
endif()

# SNMP: the decoder and conversions over the shared golden vectors (the Rust crate reads
# the same file), the configuration rules of spec §3, and an in-process loopback receive.
if(TDOT_SNMP)
  add_executable(tedge-dot-snmp tests/snmp.c)
  target_include_directories(tedge-dot-snmp PRIVATE connectors/snmp)
  target_link_libraries(tedge-dot-snmp PRIVATE tedge_dot_sdk)
  add_test(NAME snmp-vectors
           COMMAND tedge-dot-snmp
                   ${CMAKE_CURRENT_SOURCE_DIR}/../../connectors/snmp/conformance/trap-vectors.json)
endif()

# Strip in release-style builds for a honest size comparison
if(CMAKE_BUILD_TYPE STREQUAL "MinSizeRel" AND NOT APPLE)
  target_link_options(tedge-dot PRIVATE -s)
endif()
