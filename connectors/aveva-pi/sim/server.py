"""AVEVA PI Web API simulator for the tedge-dot e2e harness.

Our own code: it implements the PI Web API behaviour recorded in
doc/connectors/aveva-pi-connector-spec.md §9 (the documented parts of the reference), not a
recording of a real server. One PI Data Archive (`PISRV01`) with the points of seed.json, an AF
database (`\\\\AFSRV01\\Plant`) whose attributes are PI Point data references to those points, and
an in-memory archive that keeps growing while the simulator runs.

API (under /piwebapi):

    GET  /                                     root links (the connector's connect check)
    GET  dataservers                            every data server (`Items`)
    GET  dataservers?path=\\\\PISRV01            the data server
    GET  dataservers/{webId}/enumerationsets    digital state sets
    GET  enumerationsets/{webId}/values         a set's states
    GET  points?path=\\\\PISRV01\\<tag>           a PI point
    GET  attributes?path=\\\\AFSRV01\\Plant\\..|..  an AF attribute
    GET  streamsets/value?webId=..              current values
    GET  streamsets/recorded?webId=..           archived events (startTime, endTime, maxCount)
    GET  streamsets/channel?webId=..            Channels WebSocket (includeInitialValues,
                                                heartbeatRate)
    POST streams/{webId}/value                  write a value (updateOption, bufferOption)

`selectedFields` is honoured on every GET. WebIds are PI-shaped (`F1DP…` points, `F1Ab…`
attributes, `F1DS…` data servers, `F1MS…` digital sets) and stable across restarts.

Environment:

    SIM_PORT              listening port (default 8443 with TLS, 8080 without)
    SIM_TLS               1 (default): HTTPS with $SIM_CERT_DIR/server.pem; 0: plain HTTP
    SIM_CERT_DIR          certificates written by entrypoint.sh (default /certs)
    SIM_SERVER_CERT       server certificate name in SIM_CERT_DIR: server (default) or
                          server-nosan (no subject alternative name)
    SIM_AUTH              none (default), basic, bearer or certificate (client certificate
                          signed by $SIM_CERT_DIR/ca.pem required)
    SIM_USER / SIM_PASSWORD   Basic credentials (default piuser / pipassword)
    SIM_TOKEN             accepted Bearer token (default token-1; change with /_sim/token)
    SIM_CSRF              1 (default): POST needs X-Requested-With
    SIM_CHANNEL_POLL_MS   the server's ChannelPollingInterval (default 1000)
    SIM_REFUSE_CHANNELS   answer every Channels upgrade with this status from the start
    SIM_SEED              seed file (default seed.json beside this file)

Control API (no auth, for the test suite only), all JSON:

    POST /_sim/pause         {"on": bool}          accept requests, never answer them
    POST /_sim/fail          {"status": n|null, "only": "write"|null}   answer every (or every
                                                   write) request with that status
    POST /_sim/outage        {"on": bool}          API down (503, channels closed); the
                                                   archive keeps growing
    POST /_sim/limits        {"max_web_ids": n|null, "status": 414|400, "max_url_bytes": n|null}
    POST /_sim/channels      {"refuse": n|null, "silent": bool, "unordered": bool}
    POST /_sim/close_channels
    POST /_sim/channel_error {"message": "..."}    an {"Errors": [...]} message on every channel
    POST /_sim/set           {"tag", "value", "good"?, "questionable"?, "substituted"?,
                              "ts"?}               archive one event (default: now)
    POST /_sim/burst         {"tag", "count", "start"?, "step_ms"?, "same_ts"?}
                                                   archive many events (backfill tests)
    POST /_sim/freeze        {"tag", "on": bool}   stop generating values for a tag
    POST /_sim/token         {"token": "..."}      the Bearer token accepted from now on
    GET  /_sim/counters      requests per endpoint; POST /_sim/counters resets them
    GET  /_sim/writes        the last write body per tag
    GET  /_sim/archive?tag=  the tag's archived events
"""
import asyncio
import base64
import hashlib
import json
import math
import os
import ssl
import time
from datetime import datetime, timedelta, timezone
from urllib.parse import parse_qs

from aiohttp import WSMsgType, web

HERE = os.path.dirname(os.path.abspath(__file__))
MAX_RETURNED_ITEMS = 150000  # MaxReturnedItemsPerCall, the server default
INT_TYPES = {"Int16": (-(2**15), 2**15 - 1), "Int32": (-(2**31), 2**31 - 1)}


# ---- time -----------------------------------------------------------------------------------

def now_ms():
    return int(time.time() * 1000)


def iso(ms):
    dt = datetime.fromtimestamp(ms / 1000, tz=timezone.utc)
    return dt.strftime("%Y-%m-%dT%H:%M:%S.") + f"{ms % 1000:03d}Z"


def parse_time(text, default_ms):
    """ISO 8601, `*` (now) or `*-<n><s|m|h|d>`."""
    if text is None or text == "":
        return default_ms
    t = text.strip()
    if t.startswith("*"):
        rest = t[1:].strip()
        if not rest:
            return now_ms()
        sign = -1 if rest[0] == "-" else 1
        unit = rest[-1]
        n = float(rest[1:-1])
        scale = {"s": 1000, "m": 60000, "h": 3600000, "d": 86400000}[unit]
        return int(now_ms() + sign * n * scale)
    if t.endswith("Z"):
        t = t[:-1] + "+00:00"
    dt = datetime.fromisoformat(t)
    if dt.tzinfo is None:
        dt = dt.replace(tzinfo=timezone.utc)
    return int(round(dt.timestamp() * 1000))


# ---- WebIds ---------------------------------------------------------------------------------

def web_id(marker, key):
    """A PI-shaped Full WebId 2.0: F1 + marker + base64url of a stable 16-byte id."""
    raw = hashlib.sha256(key.encode()).digest()[:16]
    return "F1" + marker + base64.urlsafe_b64encode(raw).decode().rstrip("=")


# ---- the model ------------------------------------------------------------------------------

class Point:
    def __init__(self, spec, sim):
        self.spec = spec
        self.name = spec["name"]
        self.type = spec["type"]
        self.units = spec.get("units", "")
        self.set_name = spec.get("set")
        self.gen = spec.get("gen", "manual")
        self.every = float(spec.get("every", 1))
        self.write_denied = spec.get("write_denied", False)
        self.frozen = False
        self.path = f"\\\\{sim.data_server}\\{self.name}"
        self.web_id = web_id("DP", "point:" + self.name.upper())
        self.events = []  # dicts: seq, ts, value, good, questionable, substituted, annotated
        self.next_due = 0.0
        self.counter = 0
        self.sim = sim

    def digital(self, state):
        states = self.sim.digital_sets[self.set_name]
        for s in states:
            if s["Name"] == state or s["Value"] == state:
                return {"Name": s["Name"], "Value": s["Value"], "IsSystem": False}
        raise KeyError(state)

    def generate(self, t):
        if self.gen == "sine":
            a = float(self.spec.get("amplitude", 1))
            o = float(self.spec.get("offset", 0))
            p = float(self.spec.get("period", 60))
            return round(o + a * math.sin(2 * math.pi * t / p), 6)
        if self.gen == "counter":
            self.counter += 1
            return self.counter
        if self.gen == "cycle":
            states = self.sim.digital_sets[self.set_name]
            self.counter += 1
            s = states[self.counter % len(states)]
            return {"Name": s["Name"], "Value": s["Value"], "IsSystem": False}
        return None

    def initial_value(self):
        if self.gen == "system":
            code = {"I/O Timeout": 246, "No Data": 248, "Pt Created": 253, "Shutdown": 254}
            state = self.spec.get("state", "I/O Timeout")
            return {"Name": state, "Value": code.get(state, 246), "IsSystem": True}, False
        if self.type == "Digital":
            return self.digital(self.spec.get("initial", 0)), True
        if "initial" in self.spec:
            return self.spec["initial"], True
        return self.generate(time.time()), True


class Attribute:
    def __init__(self, spec, sim):
        self.rel = spec["path"]
        self.path = f"\\\\{sim.af_server}\\{sim.af_database}\\{self.rel}"
        self.name = self.rel.split("|")[-1]
        self.point = sim.points[spec["point"].upper()]
        self.units = spec.get("units", self.point.units)
        self.web_id = web_id("Ab", "attr:" + self.path.upper())


class Sim:
    def __init__(self, seed):
        self.data_server = seed["data_server"]
        self.af_server = seed["af_server"]
        self.af_database = seed["af_database"]
        self.digital_sets = seed["digital_sets"]
        self.server_web_id = web_id("DS", "server:" + self.data_server.upper())
        self.set_web_ids = {name: web_id("MS", "set:" + name.upper()) for name in self.digital_sets}
        self.points = {}
        for spec in seed["points"]:
            p = Point(spec, self)
            self.points[p.name.upper()] = p
        self.attributes = {}
        for spec in seed["attributes"]:
            a = Attribute(spec, self)
            self.attributes[a.path.upper()] = a
        self.streams = {p.web_id: p for p in self.points.values()}
        self.streams.update({a.web_id: a for a in self.attributes.values()})
        self.seq = 0
        # control state
        self.paused = False
        self.unpaused = None
        self.fail_status = None
        self.fail_only = None
        self.outage = False
        self.max_web_ids = None
        self.limit_status = 414
        self.max_url_bytes = None
        refuse = os.environ.get("SIM_REFUSE_CHANNELS", "")
        self.refuse_channels = int(refuse) if refuse else None
        self.silent_channels = False
        self.unordered_channels = False
        self.channels = set()
        self.counters = {}
        self.writes = {}
        self.user = os.environ.get("SIM_USER", "piuser")
        self.password = os.environ.get("SIM_PASSWORD", "pipassword")
        self.token = os.environ.get("SIM_TOKEN", "token-1")
        self.auth = os.environ.get("SIM_AUTH", "none")
        self.csrf = os.environ.get("SIM_CSRF", "1") == "1"
        self.poll_ms = int(os.environ.get("SIM_CHANNEL_POLL_MS", "1000"))

    # -- archive

    def archive(self, point, value, ts=None, good=True, questionable=None, substituted=False,
                annotated=False):
        self.seq += 1
        if questionable is None:
            questionable = bool(point.spec.get("questionable", False))
        ev = {"seq": self.seq, "ts": now_ms() if ts is None else ts, "value": value,
              "good": good and not (isinstance(value, dict) and value.get("IsSystem")),
              "questionable": questionable, "substituted": substituted, "annotated": annotated}
        point.events.append(ev)
        if ts is not None and len(point.events) > 1 and point.events[-2]["ts"] > ts:
            point.events.sort(key=lambda e: (e["ts"], e["seq"]))
        return ev

    def start(self):
        t = now_ms()
        for p in self.points.values():
            value, good = p.initial_value()
            self.archive(p, value, ts=t - 1000, good=good)
            p.next_due = time.time() + p.every

    async def generator(self):
        while True:
            await asyncio.sleep(0.05)
            t = time.time()
            for p in self.points.values():
                if p.gen in ("manual", "system") or p.frozen or t < p.next_due:
                    continue
                p.next_due += p.every
                if p.next_due < t:
                    p.next_due = t + p.every
                self.archive(p, p.generate(t))

    # -- rendering

    def value_json(self, stream, ev):
        return {
            "Timestamp": iso(ev["ts"]),
            "Value": ev["value"],
            "UnitsAbbreviation": stream.units,
            "Good": ev["good"],
            "Questionable": ev["questionable"],
            "Substituted": ev["substituted"],
            "Annotated": ev["annotated"],
        }

    def point_of(self, stream):
        return stream.point if isinstance(stream, Attribute) else stream


# ---- helpers --------------------------------------------------------------------------------

def errors(status, *messages):
    return web.json_response({"Errors": list(messages)}, status=status)


def select(doc, selected):
    """Apply PI Web API `selectedFields` (`Items.WebId;Items.Value.Timestamp`) to a document."""
    if not selected:
        return doc
    paths = [p.split(".") for p in selected.split(";") if p]

    def keep(node, prefixes):
        if isinstance(node, list):
            return [keep(n, prefixes) for n in node]
        if not isinstance(node, dict):
            return node
        out = {}
        for key, value in node.items():
            subs = [p[1:] for p in prefixes if p and p[0].lower() == key.lower()]
            if not subs:
                continue
            if any(len(s) == 0 for s in subs):
                out[key] = value
            else:
                out[key] = keep(value, subs)
        return out

    return keep(doc, paths)


def respond(request, doc, status=200):
    return web.json_response(select(doc, request.query.get("selectedFields")), status=status)


def web_ids(request):
    return parse_qs(request.query_string).get("webId", [])


def count(sim, name):
    sim.counters[name] = sim.counters.get(name, 0) + 1


# ---- middleware -----------------------------------------------------------------------------

@web.middleware
async def faults_and_auth(request, handler):
    sim = request.app["sim"]
    if request.path.startswith("/_sim/"):
        return await handler(request)
    if sim.paused:
        await sim.unpaused.wait()
    if sim.outage:
        return errors(503, "Service Unavailable (simulated outage)")
    is_write = request.method != "GET"
    if sim.fail_status and (sim.fail_only is None or (sim.fail_only == "write" and is_write)):
        return errors(sim.fail_status, f"simulated failure {sim.fail_status}")
    if sim.max_url_bytes and len(request.raw_path) > sim.max_url_bytes:
        return web.Response(status=400, content_type="text/html",
                            text="<h2>Bad Request - Request Too Long</h2>")
    ids = web_ids(request)
    if sim.max_web_ids and len(ids) > sim.max_web_ids:
        if sim.limit_status == 400:
            return web.Response(status=400, content_type="text/html",
                                text="<h2>Bad Request - Request Too Long</h2>")
        return web.Response(status=sim.limit_status, text="URI Too Long")
    # Authentication (a client certificate is checked by the TLS layer).
    header = request.headers.get("Authorization", "")
    if sim.auth == "basic":
        expected = "Basic " + base64.b64encode(f"{sim.user}:{sim.password}".encode()).decode()
        if header != expected:
            return web.json_response({"Errors": ["Unauthorized"]}, status=401,
                                     headers={"WWW-Authenticate": 'Basic realm="PI Web API"'})
    elif sim.auth == "bearer":
        if header != "Bearer " + sim.token:
            return web.json_response({"Errors": ["Unauthorized"]}, status=401,
                                     headers={"WWW-Authenticate": "Bearer"})
    if is_write and sim.csrf and "X-Requested-With" not in request.headers:
        return errors(400, "Requests that do not use the GET method must include the "
                           "X-Requested-With header (CSRF defense).")
    return await handler(request)


# ---- API ------------------------------------------------------------------------------------

async def root(request):
    count(request.app["sim"], "root")
    base = str(request.url.with_query(None)).rstrip("/")
    return respond(request, {"Links": {"Self": base + "/", "DataServers": base + "/dataservers"}})


async def dataservers(request):
    sim = request.app["sim"]
    count(sim, "dataservers")
    doc = {"WebId": sim.server_web_id, "Name": sim.data_server, "Path": f"\\\\{sim.data_server}"}
    if "path" not in request.query:
        return respond(request, {"Items": [doc]})
    path = request.query["path"]
    if path.strip("\\").upper() != sim.data_server.upper():
        return errors(404, f"Data server not found '{path}'.")
    return respond(request, doc)


async def enumerationsets(request):
    sim = request.app["sim"]
    count(sim, "enumerationsets")
    if request.match_info["web_id"] != sim.server_web_id:
        return errors(404, "Data server not found.")
    items = [{"WebId": sim.set_web_ids[n], "Name": n} for n in sim.digital_sets]
    return respond(request, {"Items": items, "Links": {}})


async def enumerationset_values(request):
    sim = request.app["sim"]
    count(sim, "enumerationvalues")
    for name, wid in sim.set_web_ids.items():
        if wid == request.match_info["web_id"]:
            items = [{"Name": s["Name"], "Value": s["Value"]} for s in sim.digital_sets[name]]
            return respond(request, {"Items": items, "Links": {}})
    return errors(404, "Enumeration set not found.")


async def points(request):
    sim = request.app["sim"]
    count(sim, "points")
    path = request.query.get("path", "")
    parts = path.lstrip("\\").split("\\", 1)
    if len(parts) != 2 or parts[0].upper() != sim.data_server.upper():
        return errors(404, f"PI Point not found '{path}'.")
    p = sim.points.get(parts[1].upper())
    if p is None:
        return errors(404, f"PI Point not found '{path}'.")
    return respond(request, {"WebId": p.web_id, "Id": 1, "Name": p.name, "Path": p.path,
                             "PointClass": "classic", "PointType": p.type,
                             "DigitalSetName": p.set_name or "", "EngineeringUnits": p.units,
                             "Step": False, "Future": False, "Links": {}})


async def attributes(request):
    sim = request.app["sim"]
    count(sim, "attributes")
    path = request.query.get("path", "")
    a = sim.attributes.get(path.upper())
    if a is None:
        return errors(404, f"AF attribute not found '{path}'.")
    p = a.point
    return respond(request, {"WebId": a.web_id, "Name": a.name, "Path": a.path,
                             "Type": "EnumerationValue" if p.type == "Digital" else p.type,
                             "DefaultUnitsNameAbbreviation": a.units,
                             "DataReferencePlugIn": "PI Point", "Links": {}})


def stream_item(sim, wid):
    s = sim.streams.get(wid)
    if s is None:
        return None, {"WebId": wid, "Errors": [f"The specified WebId '{wid}' was not found."]}
    return s, {"WebId": wid, "Name": s.name, "Path": s.path, "Links": {}}


async def streamsets_value(request):
    sim = request.app["sim"]
    count(sim, "streamsets/value")
    items = []
    for wid in web_ids(request):
        s, item = stream_item(sim, wid)
        if s is not None:
            item["Value"] = sim.value_json(s, sim.point_of(s).events[-1])
        items.append(item)
    return respond(request, {"Items": items, "Links": {}})


async def streamsets_recorded(request):
    sim = request.app["sim"]
    count(sim, "streamsets/recorded")
    try:
        max_count = int(request.query.get("maxCount", "1000"))
    except ValueError:
        return errors(400, "maxCount is not an integer.")
    if max_count > MAX_RETURNED_ITEMS:
        return errors(400, f"Parameter 'maxCount' is greater than the maximum allowed "
                           f"({MAX_RETURNED_ITEMS}).")
    start = parse_time(request.query.get("startTime"), now_ms() - 86400000)
    end = parse_time(request.query.get("endTime"), now_ms())
    items = []
    for wid in web_ids(request):
        s, item = stream_item(sim, wid)
        if s is not None:
            lo, hi = min(start, end), max(start, end)
            evs = [e for e in sim.point_of(s).events if lo <= e["ts"] <= hi]
            if end < start:
                evs.reverse()
            item["Items"] = [sim.value_json(s, e) for e in evs[:max_count]]
            item["UnitsAbbreviation"] = s.units
        items.append(item)
    return respond(request, {"Items": items, "Links": {}})


async def write_value(request):
    sim = request.app["sim"]
    count(sim, "streams/value")
    s = sim.streams.get(request.match_info["web_id"])
    if s is None:
        return errors(404, "The specified WebId was not found.")
    p = sim.point_of(s)
    if p.write_denied:
        return errors(403, "write access denied")
    try:
        body = await request.json()
    except json.JSONDecodeError:
        return errors(400, "The request body is not valid JSON.")
    if not isinstance(body, dict) or "Value" not in body:
        return errors(400, "The request body has no Value.")
    v = body["Value"]
    try:
        if p.type == "Digital":
            if isinstance(v, dict):
                v = p.digital(v.get("Name") if "Name" in v else v.get("Value"))
            else:
                v = p.digital(v)
        elif p.type in INT_TYPES:
            lo, hi = INT_TYPES[p.type]
            if isinstance(v, bool) or not isinstance(v, (int, float)) or v != int(v) or not lo <= v <= hi:
                raise ValueError(v)
            v = int(v)
        elif p.type.startswith("Float"):
            if isinstance(v, bool) or not isinstance(v, (int, float)):
                raise ValueError(v)
        elif p.type in ("String", "Timestamp"):
            if not isinstance(v, str):
                raise ValueError(v)
    except (KeyError, ValueError, TypeError):
        return errors(409, f"The value {json.dumps(body['Value'])} is not valid for "
                           f"the {p.type} point {p.name}.")
    ts = parse_time(body["Timestamp"], now_ms()) if "Timestamp" in body else None
    sim.writes[p.name] = {"body": body, "query": dict(request.query),
                          "x_requested_with": request.headers.get("X-Requested-With")}
    sim.archive(p, v, ts=ts)
    status = 202 if request.query.get("bufferOption", "BufferIfPossible") == "Buffer" else 204
    return web.Response(status=status)


async def channel(request):
    sim = request.app["sim"]
    count(sim, "streamsets/channel")
    if sim.refuse_channels:
        return errors(sim.refuse_channels, "WebSocket connections are not accepted.")
    ids = web_ids(request)
    initial = request.query.get("includeInitialValues", "false").lower() == "true"
    try:
        heartbeat = int(request.query.get("heartbeatRate", "0"))
    except ValueError:
        heartbeat = 0
    ws = web.WebSocketResponse()
    await ws.prepare(request)
    sim.channels.add(ws)
    streams = [sim.streams[w] for w in ids if w in sim.streams]
    last = {s.web_id: (sim.seq if not initial else -1) for s in streams}
    empty = 0
    try:
        while not ws.closed:
            items = []
            for s in streams:
                evs = sim.point_of(s).events
                if last[s.web_id] == -1:
                    new = evs[-1:]
                else:
                    new = [e for e in evs if e["seq"] > last[s.web_id]]
                if new:
                    last[s.web_id] = max(e["seq"] for e in new)
                    if sim.unordered_channels:
                        new = list(reversed(new))
                    items.append({"WebId": s.web_id, "Name": s.name, "Path": s.path,
                                  "Links": {}, "UnitsAbbreviation": s.units,
                                  "Items": [sim.value_json(s, e) for e in new]})
            if not sim.silent_channels:
                if items:
                    await ws.send_str(json.dumps({"Links": {}, "Items": items}))
                    empty = 0
                else:
                    empty += 1
                    if heartbeat > 0 and empty >= heartbeat:
                        await ws.send_str(json.dumps({"Links": {}, "Items": []}))
                        empty = 0
            try:
                msg = await ws.receive(timeout=sim.poll_ms / 1000)
                if msg.type in (WSMsgType.CLOSE, WSMsgType.CLOSED, WSMsgType.ERROR):
                    break
            except asyncio.TimeoutError:
                pass
    finally:
        sim.channels.discard(ws)
    return ws


# ---- control API ----------------------------------------------------------------------------

async def body_of(request):
    try:
        return await request.json()
    except json.JSONDecodeError:
        return {}


async def ctl_pause(request):
    sim = request.app["sim"]
    on = (await body_of(request)).get("on", True)
    sim.paused = bool(on)
    if on:
        sim.unpaused.clear()
    else:
        sim.unpaused.set()
    return web.json_response({"paused": sim.paused})


async def ctl_fail(request):
    sim = request.app["sim"]
    b = await body_of(request)
    sim.fail_status = b.get("status")
    sim.fail_only = b.get("only")
    return web.json_response({"status": sim.fail_status, "only": sim.fail_only})


async def close_channels(sim):
    for ws in list(sim.channels):
        await ws.close()


async def ctl_outage(request):
    sim = request.app["sim"]
    sim.outage = bool((await body_of(request)).get("on", True))
    if sim.outage:
        await close_channels(sim)
    return web.json_response({"outage": sim.outage})


async def ctl_limits(request):
    sim = request.app["sim"]
    b = await body_of(request)
    sim.max_web_ids = b.get("max_web_ids")
    sim.limit_status = int(b.get("status", 414))
    sim.max_url_bytes = b.get("max_url_bytes")
    return web.json_response({"max_web_ids": sim.max_web_ids, "status": sim.limit_status,
                              "max_url_bytes": sim.max_url_bytes})


async def ctl_channels(request):
    sim = request.app["sim"]
    b = await body_of(request)
    sim.refuse_channels = b.get("refuse", sim.refuse_channels)
    sim.silent_channels = bool(b.get("silent", sim.silent_channels))
    sim.unordered_channels = bool(b.get("unordered", sim.unordered_channels))
    return web.json_response({"refuse": sim.refuse_channels, "silent": sim.silent_channels,
                              "unordered": sim.unordered_channels})


async def ctl_close_channels(request):
    sim = request.app["sim"]
    n = len(sim.channels)
    await close_channels(sim)
    return web.json_response({"closed": n})


async def ctl_channel_error(request):
    sim = request.app["sim"]
    msg = (await body_of(request)).get("message", "[-10723] PINET: No Connection.")
    for ws in list(sim.channels):
        await ws.send_str(json.dumps({"Errors": [msg]}))
    return web.json_response({"sent": len(sim.channels)})


def point_arg(sim, b):
    p = sim.points.get(str(b.get("tag", "")).upper())
    if p is None:
        raise web.HTTPNotFound(text=f"no tag {b.get('tag')}")
    return p


def event_value(p, v):
    if p.type == "Digital" and not isinstance(v, dict):
        return p.digital(v)
    return v


async def ctl_set(request):
    sim = request.app["sim"]
    b = await body_of(request)
    p = point_arg(sim, b)
    ts = parse_time(b["ts"], now_ms()) if "ts" in b else None
    ev = sim.archive(p, event_value(p, b.get("value")), ts=ts, good=b.get("good", True),
                     questionable=b.get("questionable"), substituted=b.get("substituted", False))
    return web.json_response({"ts": iso(ev["ts"]), "seq": ev["seq"]})


async def ctl_burst(request):
    sim = request.app["sim"]
    b = await body_of(request)
    p = point_arg(sim, b)
    n = int(b.get("count", 10))
    step = int(b.get("step_ms", 1000))
    start = parse_time(b.get("start"), now_ms() - n * step)
    same = bool(b.get("same_ts", False))
    for i in range(n):
        # Event i's value is i (digital points alternate between codes 0 and 1), so a test can
        # tell every event apart; or the given `value` for all of them.
        v = b["value"] if "value" in b else (i % 2 if p.type == "Digital" else i)
        sim.archive(p, event_value(p, v), ts=start if same else start + i * step)
    return web.json_response({"count": n, "first": iso(start),
                              "last": iso(start if same else start + (n - 1) * step)})


async def ctl_freeze(request):
    sim = request.app["sim"]
    b = await body_of(request)
    p = point_arg(sim, b)
    p.frozen = bool(b.get("on", True))
    return web.json_response({"tag": p.name, "frozen": p.frozen})


async def ctl_token(request):
    sim = request.app["sim"]
    sim.token = (await body_of(request)).get("token", sim.token)
    return web.json_response({"ok": True})


async def ctl_counters(request):
    sim = request.app["sim"]
    if request.method == "POST":
        sim.counters = {}
    return web.json_response(sim.counters)


async def ctl_writes(request):
    return web.json_response(request.app["sim"].writes)


async def ctl_archive(request):
    sim = request.app["sim"]
    p = sim.points.get(request.query.get("tag", "").upper())
    if p is None:
        return web.json_response({"error": "no such tag"}, status=404)
    return web.json_response([{"ts": iso(e["ts"]), "value": e["value"], "good": e["good"]}
                              for e in p.events])


# ---- app ------------------------------------------------------------------------------------

def make_app(seed_path=None):
    with open(seed_path or os.environ.get("SIM_SEED", os.path.join(HERE, "seed.json"))) as f:
        seed = json.load(f)
    sim = Sim(seed)
    app = web.Application(middlewares=[faults_and_auth])
    app["sim"] = sim
    api = "/piwebapi"
    app.router.add_get(api + "/", root)
    app.router.add_get(api, root)
    app.router.add_get(api + "/dataservers", dataservers)
    app.router.add_get(api + "/dataservers/{web_id}/enumerationsets", enumerationsets)
    app.router.add_get(api + "/enumerationsets/{web_id}/values", enumerationset_values)
    app.router.add_get(api + "/points", points)
    app.router.add_get(api + "/attributes", attributes)
    app.router.add_get(api + "/streamsets/value", streamsets_value)
    app.router.add_get(api + "/streamsets/recorded", streamsets_recorded)
    app.router.add_get(api + "/streamsets/channel", channel)
    app.router.add_post(api + "/streams/{web_id}/value", write_value)
    for name, handler in [("pause", ctl_pause), ("fail", ctl_fail), ("outage", ctl_outage),
                          ("limits", ctl_limits), ("channels", ctl_channels),
                          ("close_channels", ctl_close_channels),
                          ("channel_error", ctl_channel_error), ("set", ctl_set),
                          ("burst", ctl_burst), ("freeze", ctl_freeze), ("token", ctl_token)]:
        app.router.add_post("/_sim/" + name, handler)
    app.router.add_route("*", "/_sim/counters", ctl_counters)
    app.router.add_get("/_sim/writes", ctl_writes)
    app.router.add_get("/_sim/archive", ctl_archive)

    async def on_startup(app):
        sim.unpaused = asyncio.Event()
        sim.unpaused.set()
        sim.start()
        app["generator"] = asyncio.create_task(sim.generator())

    async def on_cleanup(app):
        app["generator"].cancel()
        sim.unpaused.set()
        await close_channels(sim)

    app.on_startup.append(on_startup)
    app.on_cleanup.append(on_cleanup)
    return app


def ssl_context():
    if os.environ.get("SIM_TLS", "1") != "1":
        return None
    cert_dir = os.environ.get("SIM_CERT_DIR", "/certs")
    name = os.environ.get("SIM_SERVER_CERT", "server")
    ctx = ssl.create_default_context(ssl.Purpose.CLIENT_AUTH)
    ctx.load_cert_chain(os.path.join(cert_dir, name + ".pem"), os.path.join(cert_dir, name + ".key"))
    if os.environ.get("SIM_AUTH", "none") == "certificate":
        ctx.verify_mode = ssl.CERT_REQUIRED
        ctx.load_verify_locations(os.path.join(cert_dir, "ca.pem"))
    return ctx


if __name__ == "__main__":
    ctx = ssl_context()
    port = int(os.environ.get("SIM_PORT", "8443" if ctx else "8080"))
    web.run_app(make_app(), host="0.0.0.0", port=port, ssl_context=ctx, print=None)
