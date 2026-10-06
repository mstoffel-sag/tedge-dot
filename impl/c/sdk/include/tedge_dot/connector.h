/* tedge-dot C SDK — connector interface (C rendering of the Rust
 * `Connector` trait, impl/rust/crates/sdk/src/connector.rs).
 *
 * A connector is a vtable of function pointers plus opaque state. Protocol
 * modules provide a factory returning a heap-allocated tdot_connector_t;
 * tdot_connector_factory() selects one by protocol name (compile-time
 * feature-gated, like the Rust cargo features).
 */
#ifndef TDOT_CONNECTOR_H
#define TDOT_CONNECTOR_H

#include "config.h"
#include "model.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tdot_connector tdot_connector_t;

/* read_point's third outcome, besides 0 and -1: the point has nothing to read
 * on demand (an SNMP trap or varbind point), or no new data since its previous
 * read (a CAN frame not received again); *out is ignored. On a poll the
 * runtime publishes nothing and leaves the link alone; on a heartbeat read
 * (contract §5.3) the point gets no heartbeat until its next reset. */
#define TDOT_READ_NO_DATA 1

/* Sink a module hands pushed samples to, one call per sample. Supplied by the
 * runtime to drain_subscriptions() and only valid for the duration of that
 * call; the sample is borrowed and must not be retained past it. */
typedef void (*tdot_sample_sink_t)(void *ctx, tdot_device_t *dev,
                                   tdot_point_t *pt, const tdot_sample_t *s);

struct tdot_connector {
    const char *protocol;
    /* JSON capability descriptor published retained on startup. */
    const char *capabilities_json;
    /* Optional NULL-terminated list of [connection] / device.protocol_address
     * keys (any depth) a management command may not add or change, because
     * they name local files or relax security. See
     * tdot_reject_local_only_settings(). */
    const char *const *local_only_settings;
    void *state;
    /* Set by the caller before any connect_device() when nothing will
     * subscribe: a one-shot CLI read or write. The module must then not open
     * anything only push delivery needs -- above all a listening socket, which
     * the running service on the same host already holds (SNMP's notification
     * port). Zero (the calloc default) for the runtime. */
    bool no_push;

    /* Parse protocol-specific config ([connection], device.protocol_address,
     * point.address). Must fill point->proto and point->addr_json.
     * Returns 0 on success, -1 with err filled on invalid config. */
    int (*configure)(tdot_connector_t *self, tdot_config_t *cfg, char *err,
                     size_t errlen);

    /* Establish transport for one device (fills device->proto).
     * Returns 0 on success, -1 with err filled. Also used for reconnect. */
    int (*connect_device)(tdot_connector_t *self, tdot_device_t *dev,
                          char *err, size_t errlen);

    /* Read one point. Fills *out (bad samples carry an error reason) and
     * returns 0 when the transport is healthy, or -1 when the failure
     * indicates the device link is down (triggers the runtime's reconnect
     * backoff). Returns TDOT_READ_NO_DATA, leaving *out unused, when the point
     * has nothing to read on demand or no new data since its previous read. */
    int (*read_point)(tdot_connector_t *self, tdot_device_t *dev,
                      tdot_point_t *pt, tdot_sample_t *out);

    /* Execute a typed "write" command. Returns 0 on success, -1 with err. */
    int (*write_point)(tdot_connector_t *self, tdot_device_t *dev,
                       tdot_point_t *pt, const tdot_value_t *value, char *err,
                       size_t errlen);

    /* Optional: push delivery (contract §4.2), the C rendering of the Rust
     * trait's `subscribe`.
     *
     * Rust returns a stream the runtime selects on; the C runtime is one
     * thread per config with no async machinery, so the same job is split in
     * two: subscribe_device() arms the subscription, and drain_subscriptions()
     * is called from the poll loop to hand over whatever arrived since the
     * last tick. Keeping the handover on the runtime thread is what makes this
     * safe -- samples are published from the loop, and the module never
     * touches MQTT or the mosquitto handle.
     *
     * subscribe_device() must set pt->subscribed on every point it accepted
     * (and leave it false on the rest, which stay on the polling schedule).
     * It is called after each successful connect, so a reconnect re-arms the
     * subscription. Returns 0 when the device is armed -- including when it
     * accepted no points at all -- and -1 with err filled when the attempt
     * failed, in which case the runtime polls every point of the device.
     *
     * A module providing subscribe_device MUST also provide
     * drain_subscriptions. NULL hooks mean poll-only, which is the default. */
    int (*subscribe_device)(tdot_connector_t *self, tdot_device_t *dev,
                            char *err, size_t errlen);

    /* Optional: hand the runtime every sample received since the last call,
     * via sink(). Called once per loop tick for each connected device that was
     * subscribed. Must not block for longer than one tick. Returns 0 when the
     * transport is healthy, -1 when the link is down (triggers the runtime's
     * reconnect backoff, exactly as read_point does). */
    int (*drain_subscriptions)(tdot_connector_t *self, tdot_device_t *dev,
                               tdot_sample_sink_t sink, void *sink_ctx);

    /* Close one device's transport (frees device->proto). */
    void (*disconnect_device)(tdot_connector_t *self, tdot_device_t *dev);

    /* Optional: a device descriptor (transport/address details) as a JSON
     * OBJECT string, published as the link status `info` (contract status
     * schema) so flows can forward it into a digital-twin fragment — this is
     * what the c8y harness turns into c8y_ModbusDevice. Derived from the
     * configured address, so it is also available while the link is down.
     * Caller frees the string. NULL (or a NULL hook) means no descriptor. */
    char *(*device_info)(tdot_connector_t *self, const tdot_device_t *dev);

    /* Free the connector itself (per-point proto state included). */
    void (*destroy)(tdot_connector_t *self);
};

/* Protocol module factories (feature-gated at compile time). */
#ifdef TDOT_FEATURE_MODBUS
tdot_connector_t *tdot_connector_modbus_new(void);
#endif
#ifdef TDOT_FEATURE_OPCUA
tdot_connector_t *tdot_connector_opcua_new(void);
#endif
#ifdef TDOT_FEATURE_CANBUS
tdot_connector_t *tdot_connector_canbus_new(void);
#endif
#ifdef TDOT_FEATURE_CANOPEN
tdot_connector_t *tdot_connector_canopen_new(void);
#endif
#ifdef TDOT_FEATURE_PROFIBUS
tdot_connector_t *tdot_connector_profibus_new(void);
#endif
#ifdef TDOT_FEATURE_SNMP
tdot_connector_t *tdot_connector_snmp_new(void);
#endif

/* Returns NULL when the protocol is unknown or compiled out. */
tdot_connector_t *tdot_connector_factory(const char *protocol);

/* Write a requested value to a point through conn->write_point: the one path
 * every write takes (the `write` and `write-batch` verbs, the CLI `write`).
 *
 * A request carries engineering units -- the units of the sample value
 * (contract §4.2, §6.2) -- so for a typed point a numeric value is mapped back
 * through the inverse of the point's transform, and rounded to the nearest
 * integer (ties away from zero) when the datatype is an integer. bool/string
 * values and raw-mode points pass through unchanged, and connectors never see
 * the transform on write. A point with a value map (contract §4.3) has the
 * requested value mapped back to the device value first. Returns 0, or -1
 * with err filled -- including when the map or the transform has no inverse,
 * in which case nothing is written. */
int tdot_connector_write(tdot_connector_t *conn, tdot_device_t *dev,
                         tdot_point_t *pt, const tdot_value_t *value, char *err,
                         size_t errlen);

#ifdef __cplusplus
}
#endif

#endif /* TDOT_CONNECTOR_H */
