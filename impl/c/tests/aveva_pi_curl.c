/* The libcurl the AVEVA PI connector links (cmake/curl.cmake): TLS on mbedTLS, HTTP, HTTPS
 * and the WebSocket schemes, and none of the protocols or libraries the build turns off.
 *
 * And mbedTLS with thread safety (cmake/mbedtls_user_config.h): the connector runs a TLS
 * handshake on each device's worker thread, and two at once once corrupted PSA crypto's
 * global random generator and key slots. The keygen stress below does what those handshakes
 * do (a P-256 key pair for the key exchange); under ThreadSanitizer (just c-sanitize-aveva-pi)
 * a missing lock is reported deterministically. */
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include <curl/curl.h>
#include <mbedtls/build_info.h>
#include <psa/crypto.h>

static int failures;

#define CHECK(cond, ...)                                                                   \
    do {                                                                                   \
        if (!(cond)) {                                                                     \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                            \
            fprintf(stderr, __VA_ARGS__);                                                  \
            fputc('\n', stderr);                                                           \
            failures++;                                                                    \
        }                                                                                  \
    } while (0)

static int has_protocol(const curl_version_info_data *v, const char *name) {
    for (const char *const *p = v->protocols; *p; p++)
        if (strcmp(*p, name) == 0)
            return 1;
    return 0;
}

enum { KEYGEN_THREADS = 8, KEYGEN_ROUNDS = 25 };

static void *keygen(void *arg) {
    int *failed = arg;
    for (int i = 0; i < KEYGEN_ROUNDS; i++) {
        psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
        psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
        psa_set_key_bits(&attr, 256);
        psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_DERIVE);
        psa_set_key_algorithm(&attr, PSA_ALG_ECDH);
        psa_key_id_t key;
        if (psa_generate_key(&attr, &key) != PSA_SUCCESS) {
            (*failed)++;
            continue;
        }
        psa_destroy_key(key);
    }
    return NULL;
}

static void concurrent_keygen(void) {
#ifndef MBEDTLS_THREADING_C
    CHECK(0, "mbedTLS is built without MBEDTLS_THREADING_C");
#endif
    CHECK(psa_crypto_init() == PSA_SUCCESS, "psa_crypto_init failed");
    pthread_t th[KEYGEN_THREADS];
    int failed[KEYGEN_THREADS] = {0};
    for (int i = 0; i < KEYGEN_THREADS; i++)
        pthread_create(&th[i], NULL, keygen, &failed[i]);
    for (int i = 0; i < KEYGEN_THREADS; i++) {
        pthread_join(th[i], NULL);
        CHECK(failed[i] == 0, "thread %d: %d of %d key generations failed", i, failed[i],
              KEYGEN_ROUNDS);
    }
}

int main(void) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    const curl_version_info_data *v = curl_version_info(CURLVERSION_NOW);
    CHECK(v->version_num >= 0x080b00, "libcurl %s is older than 8.11", v->version);
    CHECK(v->ssl_version && strstr(v->ssl_version, "mbedTLS"), "TLS backend is %s",
          v->ssl_version ? v->ssl_version : "(none)");
    CHECK(v->features & CURL_VERSION_SSL, "no TLS");
    for (const char *want[] = {"http", "https", "ws", "wss", NULL}, **p = want; *p; p++)
        CHECK(has_protocol(v, *p), "protocol %s missing", *p);
    for (const char *unwanted[] = {"ftp", "file", "ldap", "smtp", "telnet", "dict", NULL},
                    **p = unwanted;
         *p; p++)
        CHECK(!has_protocol(v, *p), "protocol %s should be disabled", *p);
    CHECK(!(v->features & CURL_VERSION_LIBZ), "zlib should be disabled");
    CHECK(!(v->features & CURL_VERSION_HTTP2), "HTTP/2 should be disabled");
    CHECK(!(v->features & CURL_VERSION_IDN), "IDN should be disabled");
    CHECK(!(v->features & CURL_VERSION_PSL), "libpsl should be disabled");
    concurrent_keygen();
    printf("aveva-pi-curl: libcurl %s, %s\n", v->version, v->ssl_version);
    curl_global_cleanup();
    return failures ? 1 : 0;
}
