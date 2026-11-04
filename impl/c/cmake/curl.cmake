# libcurl (curl license, MIT-style) for the AVEVA PI connector: HTTPS requests and the PI Web
# API Channels WebSocket (stable in libcurl since 8.11). Built from the release tarball as a
# minimal static library on the in-tree mbedTLS (cmake/mbedtls.cmake): only HTTP, HTTPS, WS and
# WSS, and no nghttp2, libssh2, zlib, brotli, zstd, libidn2, libpsl or LDAP, so the binary
# gains no runtime dependency beyond libc and the cross build keeps its glibc floor. Same
# recipe for native, zig cross (cross/build.sh) and the e2e image.
#
# tdot_fetch_curl() defines the target CURL::libcurl. When the OPC UA block already chose an
# installed open62541 with the system mbedTLS (_tdot_mbedtls_system), curl is pointed at that
# same mbedTLS: two copies in one binary would clash.
include(FetchContent)

set(TDOT_CURL_URL "https://curl.se/download/curl-8.23.0.tar.gz"
    CACHE STRING "curl 8.23.0 release tarball (a URL or a local path)")

macro(tdot_fetch_curl)
  if(NOT _tdot_mbedtls_system)
    tdot_fetch_mbedtls()
  endif()
  set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  set(BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
  set(BUILD_LIBCURL_DOCS OFF CACHE BOOL "" FORCE)
  set(BUILD_MISC_DOCS OFF CACHE BOOL "" FORCE)
  set(ENABLE_CURL_MANUAL OFF CACHE BOOL "" FORCE)
  set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
  set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  # pkg-config would find the host's libraries in a cross build; every dependency is either
  # in-tree (mbedTLS) or off.
  set(CURL_USE_PKGCONFIG OFF CACHE BOOL "" FORCE)
  set(CURL_USE_CMAKECONFIG OFF CACHE BOOL "" FORCE)
  set(CURL_ENABLE_SSL ON CACHE BOOL "" FORCE)
  set(CURL_USE_MBEDTLS ON CACHE BOOL "" FORCE)
  set(CURL_USE_OPENSSL OFF CACHE BOOL "" FORCE)
  set(MBEDTLS_USE_STATIC_LIBS ON CACHE BOOL "" FORCE)
  # curl links a probe against mbedTLS at configure time, before the in-tree library exists:
  # mbedTLS 3.6 has DES, so the answer is known.
  set(HAVE_MBEDTLS_DES_CRYPT_ECB 1 CACHE INTERNAL "")
  # No CA bundle path is baked in: a cross build would bake in the build host's. The connector
  # names the bundle itself (tls.ca_file, else the first standard system bundle it finds).
  set(CURL_CA_BUNDLE "none" CACHE STRING "" FORCE)
  set(CURL_CA_PATH "none" CACHE STRING "" FORCE)
  foreach(_opt USE_NGHTTP2 USE_NGTCP2 USE_LIBIDN2 USE_APPLE_IDN CURL_USE_LIBPSL
               CURL_USE_LIBSSH2 CURL_USE_LIBSSH CURL_USE_GSSAPI CURL_ZLIB CURL_BROTLI CURL_ZSTD
               ENABLE_ARES USE_ECH CURL_USE_LIBUV)
    set(${_opt} OFF CACHE BOOL "" FORCE)
  endforeach()
  foreach(_proto DICT FILE FTP GOPHER IMAP LDAP LDAPS MQTT POP3 RTSP SMTP TELNET TFTP IPFS)
    set(CURL_DISABLE_${_proto} ON CACHE BOOL "" FORCE)
  endforeach()
  set(CURL_DISABLE_WEBSOCKETS OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(curl
    URL ${TDOT_CURL_URL}
    URL_HASH SHA256=e3de77cfa6367083c776a26647fc9f633b538c06c3baa73d931a252ac8f72b49
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(curl)
  if(TARGET mbedcrypto)
    add_dependencies(libcurl_static mbedtls mbedx509 mbedcrypto)
  endif()
endmacro()
