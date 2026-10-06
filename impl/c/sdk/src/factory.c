#include "tedge_dot/connector.h"
#include "tedge_dot/map.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

tdot_connector_t *tdot_connector_factory(const char *protocol) {
#ifdef TDOT_FEATURE_MODBUS
    if (strcmp(protocol, "modbus") == 0)
        return tdot_connector_modbus_new();
#endif
#ifdef TDOT_FEATURE_OPCUA
    if (strcmp(protocol, "opcua") == 0)
        return tdot_connector_opcua_new();
#endif
#ifdef TDOT_FEATURE_CANBUS
    if (strcmp(protocol, "canbus") == 0)
        return tdot_connector_canbus_new();
#endif
#ifdef TDOT_FEATURE_CANOPEN
    if (strcmp(protocol, "canopen") == 0)
        return tdot_connector_canopen_new();
#endif
#ifdef TDOT_FEATURE_PROFIBUS
    if (strcmp(protocol, "profibus") == 0)
        return tdot_connector_profibus_new();
#endif
#ifdef TDOT_FEATURE_SNMP
    if (strcmp(protocol, "snmp") == 0)
        return tdot_connector_snmp_new();
#endif
    return NULL;
}

/* The value write_point receives for a requested one (contract §4.2): see
 * tdot_connector_write. */
static int write_value(const tdot_point_t *pt, const tdot_value_t *in,
                       tdot_value_t *out, char *err, size_t errlen) {
    *out = *in;
    if (pt->mode == TDOT_MODE_RAW || in->kind != TDOT_VAL_NUM ||
        !pt->has_transform || tdot_transform_is_identity(&pt->transform))
        return 0;
    double raw;
    switch (tdot_transform_invert(&pt->transform, in->num, &raw)) {
    case 0:
        break;
    case TDOT_TRANSFORM_NOT_INVERTIBLE:
        snprintf(err, errlen, "point %s transform is not invertible (%s)", pt->id,
                 pt->transform.multiplier == 0.0 ? "multiplier 0"
                                                 : "decimal_shift out of range");
        return -1;
    default:
        snprintf(err, errlen, "point %s: value has no finite raw value under its transform",
                 pt->id);
        return -1;
    }
    out->num = tdot_datatype_is_integer(pt->datatype) ? round(raw) : raw;
    return 0;
}

int tdot_connector_write(tdot_connector_t *conn, tdot_device_t *dev,
                         tdot_point_t *pt, const tdot_value_t *value, char *err,
                         size_t errlen) {
    /* The value map first (§4.3): a write carries the mapped value, so it is
     * mapped back to the device value before the transform is inverted. */
    tdot_value_t unmapped = *value;
    if (pt->map && pt->mode == TDOT_MODE_TYPED) {
        char why[TDOT_ERR_MAX];
        if (tdot_map_invert(pt->map, value, pt->datatype, &unmapped, why, sizeof why) != 0) {
            snprintf(err, errlen, "point %s: %s", pt->id, why);
            return -1;
        }
    }
    tdot_value_t raw;
    if (write_value(pt, &unmapped, &raw, err, errlen) != 0)
        return -1;
    return conn->write_point(conn, dev, pt, &raw, err, errlen);
}
