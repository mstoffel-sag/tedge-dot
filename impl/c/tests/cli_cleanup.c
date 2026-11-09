/* Exercise CLI cleanup in-process so error exits cannot hide leaked ownership. */
#include "tedge_dot/connector.h"

static tdot_connector_t *fake_factory(const char *protocol);
static void tracked_config_free(tdot_config_t *cfg);

#define main tdot_cli_main
#define tdot_connector_factory fake_factory
#define tdot_config_free tracked_config_free
#include "../src/main.c"
#undef tdot_config_free
#undef tdot_connector_factory
#undef main

static int failures, destroyed, configs_freed, disconnected;
static bool fail_configure, fail_connect, fail_write;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            failures++;                                                        \
        }                                                                      \
    } while (0)

typedef struct {
    void *transport;
} fake_device_t;

static int configure(tdot_connector_t *self, tdot_config_t *cfg, char *err, size_t len) {
    (void)self;
    for (size_t i = 0; i < cfg->ndevices; i++) {
        cfg->devices[i].proto = calloc(1, sizeof(fake_device_t));
        if (fail_configure) {
            snprintf(err, len, "configure failed");
            return -1;
        }
    }
    return 0;
}

static int connect_device(tdot_connector_t *self, tdot_device_t *dev, char *err, size_t len) {
    (void)self;
    fake_device_t *fd = dev->proto;
    fd->transport = malloc(16);
    if (fail_connect) {
        snprintf(err, len, "connect failed");
        return -1;
    }
    return 0;
}

static void disconnect_device(tdot_connector_t *self, tdot_device_t *dev) {
    (void)self;
    CHECK(destroyed == 0 && configs_freed == 0);
    fake_device_t *fd = dev->proto;
    if (fd) {
        free(fd->transport);
        fd->transport = NULL;
    }
    disconnected++;
}

static int read_point(tdot_connector_t *self, tdot_device_t *dev, tdot_point_t *pt,
                      tdot_sample_t *sample) {
    (void)self, (void)dev, (void)pt;
    sample->value.kind = TDOT_VAL_NUM;
    sample->value.num = 42;
    return 0;
}

static int write_point(tdot_connector_t *self, tdot_device_t *dev, tdot_point_t *pt,
                       const tdot_value_t *value, char *err, size_t len) {
    (void)self, (void)dev, (void)pt, (void)value;
    if (fail_write) {
        snprintf(err, len, "write failed");
        return -1;
    }
    return 0;
}

static void destroy(tdot_connector_t *self) {
    CHECK(configs_freed == 0);
    destroyed++;
    free(self);
}

static tdot_connector_t *fake_factory(const char *protocol) {
    (void)protocol;
    tdot_connector_t *conn = calloc(1, sizeof *conn);
    conn->configure = configure;
    conn->connect_device = connect_device;
    conn->disconnect_device = disconnect_device;
    conn->read_point = read_point;
    conn->write_point = write_point;
    conn->destroy = destroy;
    return conn;
}

static void tracked_config_free(tdot_config_t *cfg) {
    CHECK(destroyed == 1);
    for (size_t i = 0; i < cfg->ndevices; i++) {
        fake_device_t *fd = cfg->devices[i].proto;
        CHECK(!fd || !fd->transport);
    }
    configs_freed++;
    tdot_config_free(cfg);
}

static void run(args_t args, bool write, int expected) {
    destroyed = configs_freed = disconnected = 0;
    int rc = write ? cmd_write(&args) : cmd_read(&args);
    CHECK(rc == expected);
    CHECK(destroyed == 1);
    CHECK(configs_freed == 1);
    CHECK(disconnected == 2);
}

int main(void) {
    char path[] = "/tmp/tdot-cli-cleanup-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) {
        perror("mkstemp");
        return 1;
    }
    FILE *fp = fdopen(fd, "w");
    if (!fp) {
        perror("fdopen");
        close(fd);
        unlink(path);
        return 1;
    }
    fputs("[connector]\nprotocol = 'fake'\n"
          "[[device]]\nname = 'd'\nprotocol_address = {}\n"
          "[[device.point]]\nid = 'rw'\ndatatype = 'uint16'\n"
          "access = 'read_write'\naddress = {}\n"
          "[[device.point]]\nid = 'ro'\ndatatype = 'uint16'\naddress = {}\n"
          "[[device]]\nname = 'unconfigured'\nprotocol_address = {}\n", fp);
    if (fclose(fp) != 0) {
        perror("fclose");
        unlink(path);
        return 1;
    }
    args_t args = {.config = path, .device = "d", .npoints = 1, .value = "42"};
    args.points[0] = "rw";
    fail_configure = true;
    run(args, false, 1);
    run(args, true, 1);
    fail_configure = false;
    args.device = "missing";
    run(args, false, 1);
    run(args, true, 1);
    args.device = "d";
    args.points[0] = "missing";
    run(args, false, 1);
    run(args, true, 1);
    args.points[0] = "ro";
    run(args, true, 1);
    args.points[0] = "rw";
    fail_connect = true;
    run(args, true, 1);
    fail_connect = false;
    fail_write = true;
    run(args, true, 1);
    fail_write = false;
    run(args, true, 0);
    run(args, false, 0);
    unlink(path);
    return failures ? 1 : 0;
}
