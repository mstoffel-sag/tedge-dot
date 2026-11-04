"""Unit tests of the PI Web API simulator (pytest, pytest-aiohttp).

    pip install -r requirements-test.txt && pytest -q
"""
import asyncio
import base64
import json
import os

import pytest

os.environ.setdefault("SIM_CHANNEL_POLL_MS", "50")

import server  # noqa: E402

API = "/piwebapi"


@pytest.fixture
async def client(aiohttp_client):
    return await aiohttp_client(server.make_app())


async def get(client, endpoint, **params):
    r = await client.get(API + endpoint, params=params)
    return r.status, await r.json()


async def web_id_of(client, tag):
    status, doc = await get(client, "/points", path=f"\\\\PISRV01\\{tag}")
    assert status == 200, doc
    return doc["WebId"]


async def test_root_and_lookup(client):
    status, doc = await get(client, "/")
    assert status == 200 and "Links" in doc
    status, doc = await get(client, "/points", path="\\\\PISRV01\\SINUSOID",
                            selectedFields="WebId;Name;PointType;EngineeringUnits")
    assert status == 200
    assert set(doc) == {"WebId", "Name", "PointType", "EngineeringUnits"}
    assert doc["WebId"].startswith("F1DP") and doc["PointType"] == "Float64"
    status, doc = await get(client, "/points", path="\\\\PISRV01\\NOPE")
    assert status == 404 and doc["Errors"]
    status, doc = await get(client, "/attributes",
                            path="\\\\AFSRV01\\Plant\\Line1\\Pump01|Discharge Pressure")
    assert status == 200 and doc["WebId"].startswith("F1Ab")
    assert doc["DefaultUnitsNameAbbreviation"] == "bar"


async def test_web_ids_are_stable(client, aiohttp_client):
    other = await aiohttp_client(server.make_app())
    assert await web_id_of(client, "TICKS") == await web_id_of(other, "TICKS")


async def test_digital_sets(client):
    status, ds = await get(client, "/dataservers", path="\\\\PISRV01")
    assert status == 200
    status, listing = await get(client, "/dataservers", selectedFields="Items.Name")
    assert status == 200 and listing == {"Items": [{"Name": "PISRV01"}]}
    status, _ = await get(client, "/dataservers", path="\\\\OTHER")
    assert status == 404
    status, sets = await get(client, f"/dataservers/{ds['WebId']}/enumerationsets")
    assert [s["Name"] for s in sets["Items"]] == ["PumpState"]
    status, values = await get(client, f"/enumerationsets/{sets['Items'][0]['WebId']}/values")
    assert [v["Name"] for v in values["Items"]] == ["Stopped", "Running", "Fault"]


async def test_streamsets_value_and_selected_fields(client):
    a, b = await web_id_of(client, "SETPOINT"), await web_id_of(client, "PUMP01.STATE")
    r = await client.get(API + "/streamsets/value",
                         params=[("webId", a), ("webId", b), ("webId", "F1DPnope"),
                                 ("selectedFields", "Items.WebId;Items.Value.Value;Items.Errors")])
    doc = await r.json()
    assert doc["Items"][0] == {"WebId": a, "Value": {"Value": 21.5}}
    assert doc["Items"][1]["Value"]["Value"] == {"Name": "Running", "Value": 1, "IsSystem": False}
    assert doc["Items"][2]["Errors"]


async def test_system_state_is_not_good(client):
    w = await web_id_of(client, "IO.TIMEOUT")
    _, doc = await get(client, "/streamsets/value", webId=w)
    value = doc["Items"][0]["Value"]
    assert value["Good"] is False and value["Value"]["IsSystem"] is True


async def test_recorded_paging_and_limits(client):
    w = await web_id_of(client, "FROZEN")
    r = await client.post("/_sim/burst", json={"tag": "FROZEN", "count": 25,
                                               "start": "2026-01-01T00:00:00Z", "step_ms": 1000})
    assert r.status == 200
    _, doc = await get(client, "/streamsets/recorded", webId=w,
                       startTime="2026-01-01T00:00:00Z", endTime="*", maxCount="10")
    items = doc["Items"][0]["Items"]
    assert len(items) == 10 and items[0]["Value"] == 0 and items[-1]["Value"] == 9
    # No continuation: the next page restarts at the last timestamp, inclusive.
    _, doc = await get(client, "/streamsets/recorded", webId=w,
                       startTime=items[-1]["Timestamp"], endTime="*", maxCount="10")
    assert doc["Items"][0]["Items"][0]["Value"] == 9
    status, doc = await get(client, "/streamsets/recorded", webId=w, maxCount="150001")
    assert status == 400


async def test_write_digital_and_csrf(client):
    w = await web_id_of(client, "PUMP01.STATE")
    r = await client.post(API + f"/streams/{w}/value", json={"Value": {"Name": "Stopped"}})
    assert r.status == 400  # no X-Requested-With
    headers = {"X-Requested-With": "XMLHttpRequest"}
    r = await client.post(API + f"/streams/{w}/value", headers=headers,
                          json={"Value": {"Name": "Stopped", "Value": 0, "IsSystem": False}})
    assert r.status == 204
    _, doc = await get(client, "/streamsets/value", webId=w)
    assert doc["Items"][0]["Value"]["Value"]["Name"] == "Stopped"
    r = await client.post(API + f"/streams/{w}/value", headers=headers,
                          json={"Value": {"Name": "Exploded"}})
    assert r.status == 409
    denied = await web_id_of(client, "READONLY")
    r = await client.post(API + f"/streams/{denied}/value", headers=headers, json={"Value": 2})
    assert r.status == 403 and "write access denied" in (await r.json())["Errors"][0]
    r = await client.post(API + f"/streams/{w}/value?bufferOption=Buffer", headers=headers,
                          json={"Value": 1})
    assert r.status == 202


async def test_basic_and_bearer_auth(aiohttp_client, monkeypatch):
    monkeypatch.setenv("SIM_AUTH", "basic")
    c = await aiohttp_client(server.make_app())
    assert (await c.get(API + "/")).status == 401
    good = "Basic " + base64.b64encode(b"piuser:pipassword").decode()
    assert (await c.get(API + "/", headers={"Authorization": good})).status == 200
    monkeypatch.setenv("SIM_AUTH", "bearer")
    c = await aiohttp_client(server.make_app())
    assert (await c.get(API + "/", headers={"Authorization": "Bearer token-1"})).status == 200
    await c.post("/_sim/token", json={"token": "token-2"})
    assert (await c.get(API + "/", headers={"Authorization": "Bearer token-1"})).status == 401
    assert (await c.get(API + "/", headers={"Authorization": "Bearer token-2"})).status == 200


async def test_limits_and_faults(client):
    ids = [await web_id_of(client, t) for t in ("SINUSOID", "TICKS", "SLOW")]
    await client.post("/_sim/limits", json={"max_web_ids": 2, "status": 414})
    r = await client.get(API + "/streamsets/value", params=[("webId", i) for i in ids])
    assert r.status == 414
    await client.post("/_sim/limits", json={"max_url_bytes": 100, "status": 400})
    r = await client.get(API + "/streamsets/value", params=[("webId", i) for i in ids])
    assert r.status == 400 and r.content_type == "text/html"
    await client.post("/_sim/limits", json={})
    await client.post("/_sim/fail", json={"status": 503, "only": "write"})
    assert (await client.get(API + "/")).status == 200
    r = await client.post(API + f"/streams/{ids[0]}/value",
                          headers={"X-Requested-With": "x"}, json={"Value": 1})
    assert r.status == 503
    await client.post("/_sim/fail", json={})
    await client.post("/_sim/outage", json={"on": True})
    assert (await client.get(API + "/")).status == 503
    await client.post("/_sim/outage", json={"on": False})
    # Requests the limits or faults turned away never reached the API, so they are not counted.
    await client.get(API + "/streamsets/value", params=[("webId", i) for i in ids])
    counters = await (await client.get("/_sim/counters")).json()
    assert counters["streamsets/value"] == 1


async def test_pause_never_answers(client):
    await client.post("/_sim/pause", json={"on": True})
    with pytest.raises(asyncio.TimeoutError):
        await asyncio.wait_for(client.get(API + "/"), 0.3)
    await client.post("/_sim/pause", json={"on": False})
    assert (await client.get(API + "/")).status == 200


async def test_channel_push_heartbeat_and_errors(client):
    w = await web_id_of(client, "FROZEN")
    ws = await client.ws_connect(API + "/streamsets/channel",
                                 params={"webId": w, "includeInitialValues": "true",
                                         "heartbeatRate": "2"})
    first = json.loads((await ws.receive(timeout=2)).data)
    assert first["Items"][0]["WebId"] == w and len(first["Items"][0]["Items"]) == 1
    heartbeat = json.loads((await ws.receive(timeout=2)).data)
    assert heartbeat == {"Links": {}, "Items": []}
    await client.post("/_sim/set", json={"tag": "FROZEN", "value": 8.5})
    msg = json.loads((await ws.receive(timeout=2)).data)
    while not msg["Items"]:
        msg = json.loads((await ws.receive(timeout=2)).data)
    assert msg["Items"][0]["Items"][0]["Value"] == 8.5
    await client.post("/_sim/channel_error", json={"message": "[-10723] PINET: No Connection."})
    msg = json.loads((await ws.receive(timeout=2)).data)
    while "Errors" not in msg:
        msg = json.loads((await ws.receive(timeout=2)).data)
    assert msg["Errors"] == ["[-10723] PINET: No Connection."]
    await ws.close()


async def test_channel_refused(client):
    await client.post("/_sim/channels", json={"refuse": 403})
    r = await client.get(API + "/streamsets/channel", params={"webId": "x"})
    assert r.status == 403
