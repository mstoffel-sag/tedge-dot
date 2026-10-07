// ot-measurement/units-state: remember the published measurement metadata, for ot-measurement.
//
//   in:  te/device/<device>///m/<type>/meta   (retained measurement metadata, e.g. units)
//   out: nothing
//
// ot-measurement publishes each measurement's units retained on <measurement topic>/meta, as one
// message holding every series of that topic. It cannot read them back, because the mapper drops
// a flow's output to its own input topics to prevent loops. This flow subscribes in its place and
// records each message in the mapper-wide store:
//
//   "ot-measurement-units:<meta topic>" -> { "<group>.<series>": { "unit": "..." }, ... }
//
// After a mapper restart the broker replays the retained messages, so ot-measurement merges a new
// unit into the units already standing. Otherwise it would republish only the series it has seen
// since the restart, and drop the units of points that rarely send a sample. An empty message
// (cleared topic) or one that is not a JSON object is recorded as no units.

const decoder = new TextDecoder();

export function onMessage(message, context) {
  let units = {};
  if (message.payload.length > 0) {
    try {
      const parsed = JSON.parse(decoder.decode(message.payload));
      if (parsed && typeof parsed === "object" && !Array.isArray(parsed)) units = parsed;
    } catch (_e) {
      // not ours, or not JSON: treat as no units
    }
  }
  context.mapper.set(`ot-measurement-units:${message.topic}`, units);
  return [];
}
