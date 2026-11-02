/* tedge-dot — Modbus connector on libmodbus (LGPL-2.1+, dynamically
 * linked). Mirrors impl/rust/crates/connector-modbus: TCP + RTU transports, the four
 * tables (coil / discrete_input / holding / input), multi-register typed
 * decode, single-point writes, quality propagation on Modbus exceptions.
 */
#include <errno.h>
#include <modbus.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cjson/cJSON.h"
#include "tedge_dot/connector.h"
#include "tedge_dot/decode.h"

typedef enum {
    TABLE_COIL,
    TABLE_DISCRETE,
    TABLE_HOLDING,
    TABLE_INPUT,
} mb_table_t;

/* Per-point parsed address (pt->proto, flat, freed by config_free). */
typedef struct {
    mb_table_t table;
    uint16_t address;
    uint32_t start_bit; /* bit-field refinement (contract §4); bit_count 0 = whole value */
    uint32_t bit_count;
    uint16_t count; /* registers or bits */
} mb_point_t;

/* Per-device state (dev->proto, flat, freed by config_free; ctx freed on
 * disconnect). */
typedef struct {
    bool tcp;
    char host[128];
    int port;
    char serial[128];
    int baudrate;
    char parity;
    int databits, stopbits;
    int unit_id;
    modbus_t *ctx; /* NULL when disconnected */
} mb_device_t;

typedef struct {
    int baudrate; /* [connection.serial] defaults */
    char parity;
    int databits, stopbits;
    /* connector.operation_timeout, pushed into libmodbus's response timeout.
     * This runtime cannot cancel a call in flight, so bounding the library is
     * what actually makes a silent peer return (contract §8.1). */
    double operation_timeout_s;
} mb_state_t;

static const char CAPABILITIES[] =
    "{\"protocol\":\"modbus\",\"version\":\"" TDOT_VERSION "\","
    "\"modes\":[\"raw\",\"typed\"],"
    "\"datatypes\":[\"bool\",\"int16\",\"uint16\","
    "\"int32\",\"uint32\",\"int64\",\"uint64\",\"float32\",\"float64\"],"
    "\"point_kinds\":[\"coil\",\"discrete_input\",\"holding_register\","
    "\"input_register\"],"
    "\"command_verbs\":[\"write\",\"write-coil\",\"write-batch\"],"
    "\"features\":[\"polling\",\"bitfield\"],\"subscribe\":false}";

static int parse_table(const char *s, mb_table_t *out) {
    if (strcmp(s, "coil") == 0)
        *out = TABLE_COIL;
    else if (strcmp(s, "discrete_input") == 0)
        *out = TABLE_DISCRETE;
    else if (strcmp(s, "holding") == 0)
        *out = TABLE_HOLDING;
    else if (strcmp(s, "input") == 0)
        *out = TABLE_INPUT;
    else
        return -1;
    return 0;
}

static int configure(tdot_connector_t *self, tdot_config_t *cfg, char *err,
                     size_t errlen) {
    mb_state_t *st = self->state;

    st->operation_timeout_s = cfg->operation_timeout_s;
    st->baudrate = 9600;
    st->parity = 'N';
    st->databits = 8;
    st->stopbits = 2;
    if (cfg->connection) {
        toml_table_t *serial = toml_table_in(cfg->connection, "serial");
        if (serial) {
            toml_datum_t d;
            if ((d = toml_int_in(serial, "baudrate")).ok)
                st->baudrate = (int)d.u.i;
            if ((d = toml_string_in(serial, "parity")).ok) {
                st->parity = d.u.s[0];
                free(d.u.s);
            }
            if ((d = toml_int_in(serial, "databits")).ok)
                st->databits = (int)d.u.i;
            if ((d = toml_int_in(serial, "stopbits")).ok)
                st->stopbits = (int)d.u.i;
        }
    }

    for (size_t i = 0; i < cfg->ndevices; i++) {
        tdot_device_t *dev = &cfg->devices[i];
        mb_device_t *mb = calloc(1, sizeof *mb);
        dev->proto = mb;
        toml_table_t *pa = dev->protocol_address;
        toml_datum_t d;

        mb->unit_id = 1;
        if ((d = toml_int_in(pa, "unit_id")).ok)
            mb->unit_id = (int)d.u.i;

        char transport[16] = "tcp";
        d = toml_string_in(pa, "transport");
        if (d.ok) {
            snprintf(transport, sizeof transport, "%s", d.u.s);
            free(d.u.s);
        }
        if (strcmp(transport, "tcp") == 0) {
            mb->tcp = true;
            d = toml_string_in(pa, "host");
            if (!d.ok) {
                snprintf(err, errlen, "device %s: tcp requires host",
                         dev->name);
                return -1;
            }
            snprintf(mb->host, sizeof mb->host, "%s", d.u.s);
            free(d.u.s);
            mb->port = 502;
            if ((d = toml_int_in(pa, "port")).ok)
                mb->port = (int)d.u.i;
        } else if (strcmp(transport, "rtu") == 0) {
            mb->tcp = false;
            d = toml_string_in(pa, "serial_port");
            if (!d.ok) {
                snprintf(err, errlen, "device %s: rtu requires serial_port",
                         dev->name);
                return -1;
            }
            snprintf(mb->serial, sizeof mb->serial, "%s", d.u.s);
            free(d.u.s);
            mb->baudrate = st->baudrate;
            mb->parity = st->parity;
            mb->databits = st->databits;
            mb->stopbits = st->stopbits;
            if ((d = toml_int_in(pa, "baudrate")).ok)
                mb->baudrate = (int)d.u.i;
            if ((d = toml_string_in(pa, "parity")).ok) {
                mb->parity = d.u.s[0];
                free(d.u.s);
            }
            if ((d = toml_int_in(pa, "databits")).ok)
                mb->databits = (int)d.u.i;
            if ((d = toml_int_in(pa, "stopbits")).ok)
                mb->stopbits = (int)d.u.i;
        } else {
            snprintf(err, errlen, "device %s: unknown transport '%s'",
                     dev->name, transport);
            return -1;
        }

        for (size_t j = 0; j < dev->npoints; j++) {
            tdot_point_t *pt = &dev->points[j];
            toml_datum_t ts = toml_string_in(pt->address, "table");
            toml_datum_t ad = toml_int_in(pt->address, "address");
            if (!ts.ok || !ad.ok) {
                snprintf(err, errlen,
                         "point %s/%s: address requires table + address",
                         dev->name, pt->id);
                if (ts.ok)
                    free(ts.u.s);
                return -1;
            }
            mb_point_t *mp = calloc(1, sizeof *mp);
            pt->proto = mp;
            if (parse_table(ts.u.s, &mp->table) != 0) {
                snprintf(err, errlen, "point %s/%s: unknown table '%s'",
                         dev->name, pt->id, ts.u.s);
                free(ts.u.s);
                return -1;
            }
            free(ts.u.s);
            mp->address = (uint16_t)ad.u.i;

            toml_datum_t bd;
            if ((bd = toml_int_in(pt->address, "start_bit")).ok)
                mp->start_bit = (uint32_t)bd.u.i;
            if ((bd = toml_int_in(pt->address, "bit_count")).ok)
                mp->bit_count = (uint32_t)bd.u.i;
            toml_datum_t cn = toml_int_in(pt->address, "count");
            if (cn.ok) {
                mp->count = (uint16_t)cn.u.i;
            } else if (mp->table == TABLE_COIL ||
                       mp->table == TABLE_DISCRETE) {
                mp->count = 1;
            } else {
                size_t bytes = tdot_datatype_len(pt->datatype);
                if (bytes == 0) {
                    snprintf(err, errlen,
                             "point %s/%s: count required (no datatype)",
                             dev->name, pt->id);
                    return -1;
                }
                mp->count = (uint16_t)((bytes + 1) / 2);
            }

            const char *table_name =
                mp->table == TABLE_COIL       ? "coil"
                : mp->table == TABLE_DISCRETE ? "discrete_input"
                : mp->table == TABLE_HOLDING  ? "holding"
                                              : "input";
            char addr[128];
            snprintf(addr, sizeof addr,
                     "{\"table\":\"%s\",\"address\":%u,\"unit_id\":%d}",
                     table_name, mp->address, mb->unit_id);
            pt->addr_json = strdup(addr);
        }
    }
    return 0;
}

static void disconnect_device(tdot_connector_t *self, tdot_device_t *dev) {
    (void)self;
    mb_device_t *mb = dev->proto;
    if (mb && mb->ctx) {
        modbus_close(mb->ctx);
        modbus_free(mb->ctx);
        mb->ctx = NULL;
    }
}

static int connect_device(tdot_connector_t *self, tdot_device_t *dev,
                          char *err, size_t errlen) {
    mb_device_t *mb = dev->proto;
    disconnect_device(self, dev);

    /* modbus_new_tcp() accepts IP literals only; the _pi variant resolves host
     * names (docker service names, DNS) like the Rust connector does. */
    char port_str[16];
    snprintf(port_str, sizeof port_str, "%d", mb->port);
    mb->ctx = mb->tcp ? modbus_new_tcp_pi(mb->host, port_str)
                      : modbus_new_rtu(mb->serial, mb->baudrate, mb->parity,
                                       mb->databits, mb->stopbits);
    if (!mb->ctx) {
        snprintf(err, errlen, "modbus context: %s", modbus_strerror(errno));
        return -1;
    }
    modbus_set_slave(mb->ctx, mb->unit_id);
    {
        /* One read/write is one request, so the per-call bound IS the response
         * timeout here. */
        mb_state_t *st = self->state;
        double t = st->operation_timeout_s;
        modbus_set_response_timeout(mb->ctx, (uint32_t)t,
                                    (uint32_t)((t - (uint32_t)t) * 1e6));
    }
    if (modbus_connect(mb->ctx) != 0) {
        snprintf(err, errlen, "connect %s: %s",
                 mb->tcp ? mb->host : mb->serial, modbus_strerror(errno));
        modbus_free(mb->ctx);
        mb->ctx = NULL;
        return -1;
    }
    return 0;
}

/* A Modbus exception (illegal address etc.) is a healthy transport answering
 * "no"; anything else (timeout, connection reset) means the link is down. */
static bool is_modbus_exception(int e) {
    return e > MODBUS_ENOBASE && e <= MODBUS_ENOBASE + 0x0B;
}

static int read_point(tdot_connector_t *self, tdot_device_t *dev,
                      tdot_point_t *pt, tdot_sample_t *out) {
    (void)self;
    mb_device_t *mb = dev->proto;
    mb_point_t *mp = pt->proto;

    if (!mb->ctx) {
        tdot_sample_bad(out, "device not connected");
        return -1;
    }

    int rc;
    if (mp->table == TABLE_COIL || mp->table == TABLE_DISCRETE) {
        uint8_t bits[TDOT_RAW_MAX];
        uint16_t n = mp->count > TDOT_RAW_MAX ? TDOT_RAW_MAX : mp->count;
        rc = (mp->table == TABLE_COIL)
                 ? modbus_read_bits(mb->ctx, mp->address, n, bits)
                 : modbus_read_input_bits(mb->ctx, mp->address, n, bits);
        if (rc >= 0) {
            memcpy(out->raw, bits, n);
            out->raw_len = n;
            out->raw_group = 1;
        }
    } else {
        /* Capped by the sample's raw buffer, and by what one Modbus request
         * may carry: past 125 registers libmodbus fails the read instead. */
        uint16_t regs[TDOT_RAW_MAX / 2];
        uint16_t cap = TDOT_RAW_MAX / 2 < MODBUS_MAX_READ_REGISTERS
                           ? TDOT_RAW_MAX / 2
                           : MODBUS_MAX_READ_REGISTERS;
        uint16_t n = mp->count > cap ? cap : mp->count;
        rc = (mp->table == TABLE_HOLDING)
                 ? modbus_read_registers(mb->ctx, mp->address, n, regs)
                 : modbus_read_input_registers(mb->ctx, mp->address, n, regs);
        if (rc >= 0) {
            for (uint16_t i = 0; i < n; i++) { /* registers as BE bytes */
                out->raw[i * 2] = (uint8_t)(regs[i] >> 8);
                out->raw[i * 2 + 1] = (uint8_t)(regs[i] & 0xff);
            }
            out->raw_len = (size_t)n * 2;
            out->raw_group = 2;
        }
    }

    if (rc < 0) {
        if (is_modbus_exception(errno)) {
            tdot_sample_bad(out, "modbus exception: %s",
                            modbus_strerror(errno));
            return 0; /* transport is fine */
        }
        tdot_sample_bad(out, "modbus error: %s", modbus_strerror(errno));
        return -1; /* transport down -> runtime reconnects */
    }

    if (pt->mode == TDOT_MODE_RAW || pt->datatype == TDOT_DT_NONE) {
        out->value.kind = TDOT_VAL_NONE; /* raw mode: wire bytes only */
        return 0;
    }

    /* Coils decode from their bit bytes directly. */
    char err[TDOT_ERR_MAX];
    if (pt->datatype == TDOT_DT_BOOL &&
        (mp->table == TABLE_COIL || mp->table == TABLE_DISCRETE)) {
        out->value.kind = TDOT_VAL_BOOL;
        out->value.b = out->raw[0] != 0;
        return 0;
    }
    if (mp->bit_count > 0) {
        /* Bit-field refinement: extract the declared bits of the (reordered)
         * integer instead of decoding the whole word. */
        out->value.kind = TDOT_VAL_NUM;
        out->value.num = (double)tdot_bitfield_extract(
            out->raw, out->raw_len, pt->endianness, pt->word_order,
            mp->start_bit, mp->bit_count);
    } else if (tdot_decode(pt->datatype, out->raw, out->raw_len,
                           pt->endianness, pt->word_order, &out->value, err,
                           sizeof err) != 0) {
        tdot_sample_bad(out, "decode error: %s", err);
        return 0;
    }
    if (out->value.kind == TDOT_VAL_NUM && pt->has_transform)
        out->value.num = tdot_transform_apply(&pt->transform, out->value.num);
    return 0;
}

static int write_point(tdot_connector_t *self, tdot_device_t *dev,
                       tdot_point_t *pt, const tdot_value_t *value, char *err,
                       size_t errlen) {
    (void)self;
    mb_device_t *mb = dev->proto;
    mb_point_t *mp = pt->proto;

    if (!mb->ctx) {
        snprintf(err, errlen, "device not connected");
        return -1;
    }

    int rc;
    if (mp->table == TABLE_COIL) {
        bool b = value->kind == TDOT_VAL_BOOL  ? value->b
                 : value->kind == TDOT_VAL_NUM ? (value->num != 0)
                                               : false;
        rc = modbus_write_bit(mb->ctx, mp->address, b);
    } else if (mp->table == TABLE_HOLDING) {
        uint8_t bytes[16];
        size_t len = sizeof bytes;
        if (tdot_encode(pt->datatype, value, pt->endianness, pt->word_order,
                        bytes, &len, err, errlen) != 0)
            return -1;
        uint16_t regs[8];
        uint16_t n = (uint16_t)((len + 1) / 2);
        for (uint16_t i = 0; i < n; i++)
            regs[i] = (uint16_t)((bytes[i * 2] << 8) | bytes[i * 2 + 1]);
        rc = (n == 1)
                 ? modbus_write_register(mb->ctx, mp->address, regs[0])
                 : modbus_write_registers(mb->ctx, mp->address, n, regs);
    } else {
        snprintf(err, errlen, "table is read-only");
        return -1;
    }

    if (rc < 0) {
        snprintf(err, errlen, "modbus %s: %s",
                 is_modbus_exception(errno) ? "exception" : "error",
                 modbus_strerror(errno));
        return -1;
    }
    return 0;
}

/* Device descriptor for the link status `info` (mirrors device_descriptor() in
 * impl/rust/crates/connector-modbus/src/lib.rs, field for field, so the c8y_ModbusDevice
 * twin fragment the registration flow builds from it is the same in both
 * implementations). The RTU serial options are read from the configured address
 * rather than the resolved defaults, which is what the Rust connector reports. */
static char *device_info(tdot_connector_t *self, const tdot_device_t *dev) {
    (void)self;
    const mb_device_t *mb = dev->proto;
    if (!mb)
        return NULL; /* not configured yet */

    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "protocol", "modbus");
    if (mb->tcp) {
        cJSON_AddStringToObject(obj, "transport", "tcp");
        cJSON_AddStringToObject(obj, "host", mb->host);
        cJSON_AddNumberToObject(obj, "port", mb->port);
        cJSON_AddNumberToObject(obj, "unit_id", mb->unit_id);
    } else {
        cJSON_AddStringToObject(obj, "transport", "rtu");
        cJSON_AddStringToObject(obj, "serial_port", mb->serial);
        cJSON_AddNumberToObject(obj, "unit_id", mb->unit_id);
        /* Unset options are reported as null, as the Rust connector does. */
        toml_table_t *pa = dev->protocol_address;
        toml_datum_t d;
        if (pa && (d = toml_int_in(pa, "baudrate")).ok)
            cJSON_AddNumberToObject(obj, "baudrate", (double)d.u.i);
        else
            cJSON_AddNullToObject(obj, "baudrate");
        if (pa && (d = toml_string_in(pa, "parity")).ok) {
            cJSON_AddStringToObject(obj, "parity", d.u.s);
            free(d.u.s);
        } else {
            cJSON_AddNullToObject(obj, "parity");
        }
        if (pa && (d = toml_int_in(pa, "stopbits")).ok)
            cJSON_AddNumberToObject(obj, "stopbits", (double)d.u.i);
        else
            cJSON_AddNullToObject(obj, "stopbits");
        if (pa && (d = toml_int_in(pa, "databits")).ok)
            cJSON_AddNumberToObject(obj, "databits", (double)d.u.i);
        else
            cJSON_AddNullToObject(obj, "databits");
    }
    char *json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    return json;
}

static void destroy(tdot_connector_t *self) {
    free(self->state);
    free(self);
}

/* A TCP device's host and port; a serial bus carries one request at a time
 * (contract §5.6). As the Rust module's endpoint. */
static int endpoint(tdot_connector_t *self, const tdot_device_t *dev, char *buf, size_t len) {
    (void)self;
    const mb_device_t *mb = dev->proto;
    if (!mb)
        return -1;
    if (!mb->tcp) {
        snprintf(buf, len, "%s", mb->serial);
        return 1;
    }
    size_t i = 0;
    for (; mb->host[i] && i + 1 < len; i++)
        buf[i] = (char)tolower((unsigned char)mb->host[i]);
    snprintf(buf + i, len - i, ":%d", mb->port);
    return 0;
}

tdot_connector_t *tdot_connector_modbus_new(void) {
    tdot_connector_t *c = calloc(1, sizeof *c);
    c->protocol = "modbus";
    c->capabilities_json = CAPABILITIES;
    c->state = calloc(1, sizeof(mb_state_t));
    c->configure = configure;
    c->connect_device = connect_device;
    c->read_point = read_point;
    /* Each device has its own modbus_t (dev->proto) and the read path only
     * reads the shared timeout (contract §5.6). */
    c->concurrent_reads = true;
    c->endpoint = endpoint;
    c->write_point = write_point;
    c->disconnect_device = disconnect_device;
    c->device_info = device_info;
    c->destroy = destroy;
    return c;
}
