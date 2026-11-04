/* Additions to mbedTLS's default configuration (mbedtls/mbedtls_config.h), for every
 * translation unit that includes an mbedTLS header: mbedTLS itself, libcurl, open62541 and
 * the connectors (cmake/mbedtls.cmake defines MBEDTLS_USER_CONFIG_FILE project-wide).
 *
 * Threading: the AVEVA PI connector runs TLS on one worker thread per subscribed device, and
 * the TLS 1.3 code uses PSA crypto's global state (its key slots and its random generator).
 * Without these, two handshakes at once corrupt that state: "CTR_DRBG - The entropy source
 * failed", then a heap corruption abort. The setting changes the layout of mbedTLS structures,
 * so it must be the same in every unit, hence one file included everywhere. */
#define MBEDTLS_THREADING_C
#define MBEDTLS_THREADING_PTHREAD
