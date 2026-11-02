// ot-event-series: raise thin-edge.io events from one measurement series.
//
// Direction: thin-edge.io data model -> thin-edge.io data model.
//   in:  te/device/<device>///m/<group>                   (measurements: the `series` param)
//   out: te/device/<device>///e/<event_type>              (thin-edge event)
//
// The params watch ONE series and raise an event on every change of it, its first value
// included. It is off while `series` is empty. Per-signal events are declared on the connector
// point instead (`meta.event`, see ot-event), which works for samples of any value and needs no
// flow on the measurements.
//
// State (context.script):
//   "<event topic>:last"             -> last value of the measurement series

const decoder = new TextDecoder();

function onMeasurement(message, payload, context) {
  const cfg = context.config || {};
  // Off unless a series is configured: the flow ships active, and only declarations drive it then.
  const series = cfg.series || "";
  if (!series) return [];

  // Group defaults to the m/<group> segment of the source topic, so this flow follows whatever
  // protocol produced the measurement.
  const group = cfg.group || message.topic.split("/")[6] || "value";
  const eventType = cfg.event_type || "ot_event";
  const text = cfg.text || "OT value changed";

  // Extract the series value, tolerating both { series: v } and { series: { value: v } }.
  const node = payload?.[group]?.[series];
  const value = typeof node === "object" && node !== null ? node.value : node;
  if (value === undefined || value === null) return [];

  // Event topic derived from the device prefix of the incoming measurement topic.
  // e.g. "te/device/plc1///m/modbus" -> "te/device/plc1///e/<event_type>"
  const devicePrefix = message.topic.split("/").slice(0, 5).join("/");
  const eventTopic = `${devicePrefix}/e/${eventType}`;

  // Emit only when the value changed since the last seen one (per device+type).
  const key = `${eventTopic}:last`;
  const last = context.script.get(key);
  if (last !== undefined && last !== null && last === value) return [];
  context.script.set(key, value);

  return [{
    topic: eventTopic,
    payload: JSON.stringify({
      text,
      time: payload.time,
    }),
  }];
}

export function onMessage(message, context) {
  let payload;
  try {
    payload = JSON.parse(decoder.decode(message.payload));
  } catch (_e) {
    return [];
  }
  if (!payload || typeof payload !== "object") return [];
  return onMeasurement(message, payload, context);
}
