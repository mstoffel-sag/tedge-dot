// ot-alarm-series: raise and clear a thin-edge.io alarm from one measurement series.
//
// Direction: thin-edge.io data model -> thin-edge.io data model.
//   in:  te/device/<device>///m/<group>                   (measurements: the `series` param)
//   out: te/device/<device>///a/<alarm_type>              (retained: the alarm, or "" to clear)
//
// The params watch ONE series with a threshold and hysteresis: the alarm is raised at or above
// `threshold` and cleared below `threshold - hysteresis`. It is off while `series` is empty.
// Per-signal alarms are declared on the connector point instead (`meta.alarm`, see ot-alarm),
// which works for samples of any value and needs no flow on the measurements.
//
// Alarms are retained, so they outlive this flow's in-memory state. After a mapper restart an
// alarm's state is first taken from its retained message, which the companion flow
// alarm-state.js records: an alarm still standing is not raised again (Cumulocity counts every
// repeat of an active alarm as a new occurrence). An alarm with no retained message is unknown,
// and the first reading settles it by publishing it, raised OR cleared.
//
// State:
//   context.script "<alarm topic>:active"   -> true / false
//   context.mapper "ot-alarm-retained:<alarm topic>" -> true / false, from alarm-state.js

const decoder = new TextDecoder();

// A stored true/false, or undefined when unknown (a missing key reads back as undefined or null
// depending on the runtime).
function knownBool(v) {
  return typeof v === "boolean" ? v : undefined;
}

// Whether the retained alarm on `topic` is standing, as alarm-state.js saw it: undefined when it
// has seen nothing for the topic.
function retainedState(context, topic) {
  return knownBool(context.mapper.get(`ot-alarm-retained:${topic}`));
}

function clearMessage(topic) {
  return { topic, payload: "", mqtt: { retain: true, qos: 1 } };
}

function onMeasurement(message, payload, context) {
  const cfg = context.config || {};
  // Off unless a series is configured: the flow ships active, and only declarations drive it then.
  const series = cfg.series || "";
  if (!series) return [];

  // Group defaults to the m/<group> segment of the source topic, so this flow follows whatever
  // protocol produced the measurement.
  const group = cfg.group || message.topic.split("/")[6] || "value";
  const threshold = Number(cfg.threshold ?? 70);
  const hysteresis = Number(cfg.hysteresis ?? 5);
  const severity = cfg.severity || "major";
  const alarmType = cfg.alarm_type || "ot_alarm";
  const text = cfg.text || "Value exceeded threshold";

  // Extract the series value, tolerating both { series: v } and { series: { value: v } }.
  const node = payload?.[group]?.[series];
  const value = typeof node === "object" && node !== null ? node.value : node;
  if (typeof value !== "number") return [];

  // Alarm topic derived from the device prefix of the incoming measurement topic.
  // e.g. "te/device/plc1///m/modbus" -> "te/device/plc1///a/<alarm_type>"
  const devicePrefix = message.topic.split("/").slice(0, 5).join("/");
  const alarmTopic = `${devicePrefix}/a/${alarmType}`;

  const clearBelow = threshold - hysteresis;
  const key = `${alarmTopic}:active`;
  // Unknown until the retained alarm or the first reading after a (re)start settles it, see the
  // header.
  let active = knownBool(context.script.get(key));
  if (active === undefined) active = retainedState(context, alarmTopic);

  if (value >= threshold) {
    context.script.set(key, true);
    if (active === true) return []; // already raised; no redundant publish
    return [{
      topic: alarmTopic,
      payload: JSON.stringify({
        severity,
        text: `${text} (${value} >= ${threshold})`,
        time: payload.time,
      }),
      mqtt: { retain: true, qos: 1 },
    }];
  }

  if (value < clearBelow) {
    context.script.set(key, false);
    if (active === false) return []; // already clear
    return [clearMessage(alarmTopic)];
  }

  // Inside the hysteresis band: no state change.
  return [];
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
