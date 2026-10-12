/* OPC UA namespace tables (openspec change opcua-namespace-table): the
 * configuration errors of a device's and a library's `namespaces`. Mirrors
 * impl/rust/crates/connector-opcua/tests/namespaces.rs
 * `table_errors_are_configuration_errors` and the unit tests of namespaces.rs,
 * so both builds refuse the same files with the same words. Translation itself
 * is covered by connectors/opcua/tests/opcua_namespace_e2e.robot, which runs
 * against both builds.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "tedge_dot/config.h"
#include "tedge_dot/connector.h"

static int failures = 0;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            failures++;                                                        \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                        \
            printf(__VA_ARGS__);                                               \
            printf("\n");                                                      \
        }                                                                      \
    } while (0)

static char dir[] = "/tmp/tdot-opcua-ns-XXXXXX";

static void write_file(const char *rel, const char *text) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, rel);
    FILE *fp = fopen(path, "w");
    fputs(text, fp);
    fclose(fp);
}

/* Load `config` (with the library search path pinned to the scratch dir) and
 * configure the OPC UA module on it. Returns the module's error, "" when it
 * accepted the config. */
static const char *configure(const char *namespaces, const char *node_id,
                             const char *library_namespaces) {
    static char err[512];
    char text[2048];
    snprintf(text, sizeof text,
             "[connector]\nprotocol = \"opcua\"\npoint_library_path = [\"%s\"]\n\n"
             "[[device]]\nname = \"d\"\n%s"
             "protocol_address = { endpoint = \"opc.tcp://127.0.0.1:1/\"%s%s }\n"
             "  [[device.point]]\n  id = \"p\"\n  datatype = \"float64\"\n"
             "  address = { node_id = \"%s\" }\n",
             dir, library_namespaces ? "points_from = [\"lib\"]\n" : "",
             namespaces ? ", namespaces = " : "", namespaces ? namespaces : "", node_id);
    write_file("c.toml", text);
    if (library_namespaces) {
        snprintf(text, sizeof text,
                 "[library]\nprotocol = \"opcua\"\nnamespaces = %s\n\n"
                 "[[point]]\nid = \"q\"\ndatatype = \"float64\"\n"
                 "address = { node_id = \"ns=1;i=1\" }\n",
                 library_namespaces);
        write_file("opcua/lib.toml", text);
    }
    char path[512];
    snprintf(path, sizeof path, "%s/c.toml", dir);
    err[0] = '\0';
    tdot_config_t *cfg = tdot_config_load(path, err, sizeof err);
    if (!cfg)
        return err;
    tdot_connector_t *conn = tdot_connector_opcua_new();
    if (conn->configure(conn, cfg, err, sizeof err) == 0)
        err[0] = '\0';
    tdot_config_free(cfg);
    conn->destroy(conn);
    return err;
}

int main(void) {
    if (!mkdtemp(dir)) {
        perror("mkdtemp");
        return 1;
    }
    char sub[600];
    snprintf(sub, sizeof sub, "%s/opcua", dir);
    if (mkdir(sub, 0700) != 0) {
        perror("mkdir");
        return 1;
    }

    const char *e = configure("[\"urn:acme:plc\"]", "ns=1;i=1", NULL);
    CHECK(strstr(e, "device 'd' protocol_address: namespaces entry 0 must be "
                    "'http://opcfoundation.org/UA/', found 'urn:acme:plc'"),
          "wrong entry 0: '%s'", e);
    e = configure("[]", "ns=1;i=1", NULL);
    CHECK(strstr(e, "namespaces entry 0 must be 'http://opcfoundation.org/UA/', found none"),
          "empty table: '%s'", e);
    e = configure("\"urn:acme:plc\"", "ns=1;i=1", NULL);
    CHECK(strstr(e, "namespaces must be an array of namespace URI strings"), "not an array: '%s'", e);
    e = configure("[\"http://opcfoundation.org/UA/\", 3]", "ns=1;i=1", NULL);
    CHECK(strstr(e, "namespaces must be an array of namespace URI strings"), "a number: '%s'", e);
    e = configure("[\"http://opcfoundation.org/UA/\", \"urn:x\"]", "ns=5;i=1", NULL);
    CHECK(strstr(e, "point 'p': namespace index 5 is not in the namespace table (2 entries)"),
          "index outside: '%s'", e);
    /* Namespace 0 is never translated, so it needs no entry beyond 0. */
    e = configure("[\"http://opcfoundation.org/UA/\"]", "i=2258", NULL);
    CHECK(!*e, "a namespace-0 id must be accepted: '%s'", e);
    e = configure("[\"http://opcfoundation.org/UA/\", \"urn:x\"]", "ns=1;s=Temperature", NULL);
    CHECK(!*e, "a valid table must be accepted: '%s'", e);
    /* A library's table is checked for the library's points. */
    e = configure(NULL, "ns=0;i=2258", "[\"urn:x\"]");
    CHECK(strstr(e, "point 'q': [library] namespaces entry 0 must be"), "library table: '%s'", e);
    e = configure(NULL, "ns=0;i=2258", "[\"http://opcfoundation.org/UA/\", \"urn:x\"]");
    CHECK(!*e, "a valid library table must be accepted: '%s'", e);

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("opcua namespace tables: all checks passed\n");
    return 0;
}
