"""Tests of plugin-to-tedge-dot.py against the PI Web API simulator (pytest).

    just test-aveva-pi-python

The generated config of the main case is compared with testdata/plugin-expected.toml, which the
Rust connector loads in impl/rust/crates/connector-aveva-pi/tests/config.rs, so a change to the
script's output that the connector would refuse fails one of the two. Regenerate the file with
UPDATE_GOLDEN=1 after a deliberate change.
"""
import asyncio
import base64
import importlib.util
import json
import os
import pathlib
import sys
import threading

import pytest
from aiohttp import web

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "sim"))
import server  # noqa: E402

GOLDEN = HERE / "testdata" / "plugin-expected.toml"
# The golden file names this instead of the simulator's ephemeral port.
GOLDEN_URL = "https://pi.example.com/piwebapi"

spec = importlib.util.spec_from_file_location("plugin_to_tedge_dot", HERE / "plugin-to-tedge-dot.py")
script = importlib.util.module_from_spec(spec)
spec.loader.exec_module(script)

USER = "CORP\\pi-reader"
PASSWORD = "pi-password-1"


@pytest.fixture(scope="module")
def pi_url():
    """The simulator on 127.0.0.1 (plain HTTP, Basic auth), in a thread of its own."""
    env = {"SIM_AUTH": "basic", "SIM_USER": USER, "SIM_PASSWORD": PASSWORD, "SIM_TLS": "0"}
    saved = {k: os.environ.get(k) for k in env}
    os.environ.update(env)
    loop = asyncio.new_event_loop()
    runner = web.AppRunner(server.make_app())
    loop.run_until_complete(runner.setup())
    site = web.TCPSite(runner, "127.0.0.1", 0)
    loop.run_until_complete(site.start())
    port = site._server.sockets[0].getsockname()[1]
    thread = threading.Thread(target=loop.run_forever, daemon=True)
    thread.start()
    try:
        yield f"http://127.0.0.1:{port}/piwebapi"
    finally:
        loop.call_soon_threadsafe(loop.stop)
        thread.join(5)
        loop.run_until_complete(runner.cleanup())
        loop.close()
        for k, v in saved.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v


def run(tmp_path, monkeypatch, capsys, pi_url, tags, *extra, password=PASSWORD, poll=30):
    """Write the plugin's two files, run the script in tmp_path; (exit code, stdout, stderr)."""
    monkeypatch.chdir(tmp_path)
    (tmp_path / "pi_config.json").write_text(json.dumps({
        "PI_URL": pi_url + "/", "PI_USER": USER,
        "PI_PASSWORD": base64.b64encode(password.encode()).decode(),
        "POLL_INTERVAL": poll,
    }))
    (tmp_path / "datapoints.json").write_text(json.dumps(tags))
    code = 0
    try:
        script.main(["--pi-config", "pi_config.json", "--datapoints", "datapoints.json",
                     "--device", "pi-plant1", *extra])
    except SystemExit as e:
        code = e.code
    out, err = capsys.readouterr()
    return code, out, err


def test_generates_the_golden_config(tmp_path, monkeypatch, capsys, pi_url):
    tags = ["SINUSOID", "REACTOR01.TEMP", "TICKS", "COUNT16", "PUMP01.STATE", "BATCH.ID",
            "LAST.CHANGE", "NOPE"]
    code, out, err = run(tmp_path, monkeypatch, capsys, pi_url, tags,
                         "--ca-file", "/etc/tedge/plugins/ot/certs/pi-ca.pem")
    assert code in (0, None), err
    out = out.replace(pi_url, GOLDEN_URL)
    if os.environ.get("UPDATE_GOLDEN") == "1":
        GOLDEN.write_text(out)
    assert out == GOLDEN.read_text()
    assert "8 points written (1 not found on PISRV01)" in err
    assert "NOPE: not found on PISRV01; written as float64" in err
    # The simulator here is plain HTTP, which the connector refuses with Basic auth.
    assert "is plain HTTP: the connector refuses Basic auth over it" in err


def test_password_is_never_written(tmp_path, monkeypatch, capsys, pi_url):
    code, out, _ = run(tmp_path, monkeypatch, capsys, pi_url, ["SINUSOID"])
    assert code in (0, None)
    assert PASSWORD not in out
    assert 'password_file = "/etc/tedge/plugins/ot/secrets/aveva-pi.password"' in out


def test_backslash_user_and_quoted_tag_stay_valid_toml(tmp_path, monkeypatch, capsys, pi_url):
    tomllib = pytest.importorskip("tomllib")
    code, out, _ = run(tmp_path, monkeypatch, capsys, pi_url, ['ODD"TAG'], "--device", 'a\\b')
    assert code in (0, None)
    doc = tomllib.loads(out)
    assert doc["connection"]["auth"]["username"] == USER
    assert doc["device"][0]["name"] == "a\\b"
    assert doc["device"][0]["point"][0]["address"] == {"tag": 'ODD"TAG'}


def test_data_server_option_skips_the_listing(tmp_path, monkeypatch, capsys, pi_url):
    code, out, err = run(tmp_path, monkeypatch, capsys, pi_url, ["SINUSOID"],
                         "--data-server", "OTHERSRV")
    assert code in (0, None)
    assert 'data_server = "OTHERSRV"' in out
    assert "1 not found on OTHERSRV" in err


def test_colliding_point_ids_warn(tmp_path, monkeypatch, capsys, pi_url):
    code, _, err = run(tmp_path, monkeypatch, capsys, pi_url, ["REACTOR01.TEMP", "REACTOR01_TEMP"])
    assert code in (0, None)
    assert "both become point id 'REACTOR01_TEMP'" in err


def test_wrong_password_fails_instead_of_marking_tags_missing(tmp_path, monkeypatch, capsys, pi_url):
    code, out, _ = run(tmp_path, monkeypatch, capsys, pi_url, ["SINUSOID"], password="wrong")
    assert isinstance(code, str) and "401" in code, code
    assert out == ""
    code, out, _ = run(tmp_path, monkeypatch, capsys, pi_url, ["SINUSOID"],
                       "--data-server", "PISRV01", password="wrong")
    assert isinstance(code, str) and "looking up SINUSOID" in code and "401" in code, code


def test_unreachable_server_fails_with_a_message(tmp_path, monkeypatch, capsys):
    code, _, _ = run(tmp_path, monkeypatch, capsys, "http://127.0.0.1:9/piwebapi", ["SINUSOID"])
    assert isinstance(code, str) and code.startswith("cannot list the data servers"), code
