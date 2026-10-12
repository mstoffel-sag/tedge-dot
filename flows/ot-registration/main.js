// ot-registration: register an OT device as a thin-edge.io child device the first time the
// connector reports its link as connected.
//
// Direction: OT protocol format -> thin-edge.io data model.
//   in:  te/device/<device>/ot/<protocol>/status/link  (connector link status)
//   out: te/device/<device>//                          (retained child-device registration)
//
// Protocol-neutral: works for any connector. The connector only reports link status per device;
// this flow turns the first "connected" sighting into a retained registration so the cloud
// mappers create the child device.
//
// It also reads each connector's capability descriptor
//   in:  te/device/main/service/<service>/ot/capabilities   (retained, contract §7)
//   out: te/device/<device>///cmd/ot_call_method             (retained, {"methods":[...]})
// and advertises the `ot_call_method` command on every device that has methods (`call_method`
// points), listing them, so the cloud mapper routes method-call operations to it and a consumer
// sees which methods the device has. A device registers on its first connected link, which may
// come before or after the descriptor: the capability is published once both are known, and
// cleared (an empty retained message) when a later descriptor no longer lists methods for it.

const decoder = new TextDecoder();

// The thin-edge command type `call-method` is reached through (ot-command-forward strips `ot_`
// and turns `_` into `-`).
const METHOD_CAPABILITY = "ot_call_method";

function methodCapability(device, methods) {
  return {
    topic: `te/device/${device}///cmd/${METHOD_CAPABILITY}`,
    payload: methods ? JSON.stringify({ methods }) : "",
    mqtt: { retain: true, qos: 1 },
  };
}

// A capability descriptor: remember each device's methods, publish the capability of every
// registered device whose list changed, and clear it on devices that no longer have any.
function onCapabilities(service, payload, context) {
  let descriptor;
  try {
    descriptor = JSON.parse(payload);
  } catch (_e) {
    return [];
  }
  const byDevice = {};
  for (const m of Array.isArray(descriptor?.methods) ? descriptor.methods : []) {
    if (typeof m?.device !== "string" || typeof m?.point !== "string") continue;
    const { device, ...method } = m;
    (byDevice[device] ||= []).push(method);
  }
  const listKey = `method-devices:${service}`;
  const previous = context.mapper.get(listKey) || [];
  const out = [];
  for (const device of previous) {
    if (byDevice[device]) continue;
    context.mapper.set(`methods:${device}`, null);
    if (context.mapper.get(`registered:${device}`)) out.push(methodCapability(device, null));
  }
  for (const [device, methods] of Object.entries(byDevice)) {
    const json = JSON.stringify(methods);
    if (context.mapper.get(`methods:${device}`) === json) continue;
    context.mapper.set(`methods:${device}`, json);
    if (context.mapper.get(`registered:${device}`)) out.push(methodCapability(device, methods));
  }
  context.mapper.set(listKey, Object.keys(byDevice));
  return out;
}

export function onMessage(message, context) {
  // Topic shape: te/device/main/service/<service>/ot/capabilities
  if (message.topic.endsWith("/ot/capabilities")) {
    return onCapabilities(message.topic.split("/")[4], decoder.decode(message.payload), context);
  }
  // Topic shape: te/device/<device>/ot/<protocol>/status/link
  const parts = message.topic.split("/");
  const device = parts[2];
  const protocol = parts[4];
  if (!device || device === "main") return []; // never register the main device as a child

  let status = "connected";
  let info = null;
  let declaredType = null;
  try {
    const payload = JSON.parse(decoder.decode(message.payload));
    if (typeof payload?.status === "string") status = payload.status;
    if (payload && typeof payload.info === "object" && payload.info !== null) info = payload.info;
    // The device type the connector was configured with (contract §3.1) — what the points
    // describe, which is a far better entity type than "<protocol>-device".
    if (typeof payload?.type === "string" && payload.type) declaredType = payload.type;
  } catch (_e) {
    // tolerate empty/non-JSON payloads
  }
  if (status !== "connected") return []; // only register on a healthy link

  const deviceType = declaredType || context.config?.device_type || `${protocol}-device`;

  // Register each device only once per mapper lifetime.
  const key = `registered:${device}`;
  if (context.mapper.get(key)) return [];
  context.mapper.set(key, true);

  const out = [{
    topic: `te/device/${device}//`,
    payload: JSON.stringify({
      "@type": "child-device",
      name: device,
      type: deviceType,
      "ot-protocol": protocol,
    }),
    mqtt: { retain: true, qos: 1 },
  }];

  // Advertise the generic OT command capabilities on the device so the cloud mapper routes the
  // matching operations (ot_write backs c8y_SetRegister, ot_write_coil backs c8y_SetCoil,
  // parameter_update backs c8y_ParameterUpdate through the tedge-parameter-plugin's template).
  // Each capability is a retained empty message on te/device/<device>///cmd/<type>.
  const caps = context.config?.command_capabilities || "ot_write,ot_write_coil,parameter_update";
  for (const cap of String(caps).split(",").map((c) => c.trim()).filter((c) => c)) {
    out.push({
      topic: `te/device/${device}///cmd/${cap}`,
      payload: "{}",
      mqtt: { retain: true, qos: 1 },
    });
  }

  // The device's methods, when its connector's descriptor arrived first.
  const methods = context.mapper.get(`methods:${device}`);
  if (methods) out.push(methodCapability(device, JSON.parse(methods)));

  // Optionally publish the connector's device descriptor as a digital-twin fragment
  // (parity with the legacy c8y_ModbusDevice twin). Opt-in via params.twin_fragment.
  const twinFragment = context.config?.twin_fragment || "";
  if (twinFragment && info) {
    out.push({
      topic: `te/device/${device}///twin/${twinFragment}`,
      payload: JSON.stringify(info),
      mqtt: { retain: true, qos: 1 },
    });
  }

  return out;
}
