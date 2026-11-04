#!/usr/bin/env python3
"""Independent reference decoder for the AVEVA PI value golden vectors.

Implements doc/connectors/aveva-pi-connector-spec.md §4 (decoding a PI value) a third time, in
Python, so the Rust and C decoders are checked against something neither of them produced.

    gen_decode_vectors.py > connectors/aveva-pi/conformance/decode-vectors.json

Each vector is { name, point_type, datatype, mode, value, expect }:

- `value` is a PI value object as PI Web API returns it ({Timestamp, Value, Good, ...}).
- `expect` is { quality, value?, raw?, error?, flags? }: `value` for a good typed sample, `raw`
  (the canonical JSON text of `Value`) in raw mode, `error` for a bad sample, and `flags` when PI
  set any of Questionable, Substituted or Annotated.
"""
import json
import math
import struct
import sys

INT_RANGES = {
    "int8": (-(2**7), 2**7 - 1),
    "uint8": (0, 2**8 - 1),
    "int16": (-(2**15), 2**15 - 1),
    "uint16": (0, 2**16 - 1),
    "int32": (-(2**31), 2**31 - 1),
    "uint32": (0, 2**32 - 1),
    "int64": (-(2**63), 2**63 - 1),
    "uint64": (0, 2**64 - 1),
}
SAFE = 2**53


# ---- canonical JSON text (spec §4, raw mode) --------------------------------------------------

def number_text(n):
    """Integral numbers without a fraction (below 2^53), else the shortest round-trip form."""
    if isinstance(n, bool):
        raise TypeError("bool is not a number")
    if isinstance(n, int):
        return str(n)
    if math.isfinite(n) and n == math.floor(n) and abs(n) < SAFE:
        return str(int(n))
    return repr(n)


def canonical(v):
    """Compact JSON with object keys sorted and numbers as number_text."""
    if v is None:
        return "null"
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, (int, float)):
        return number_text(v)
    if isinstance(v, str):
        return json.dumps(v, ensure_ascii=False)
    if isinstance(v, list):
        return "[" + ",".join(canonical(x) for x in v) + "]"
    if isinstance(v, dict):
        return "{" + ",".join(json.dumps(k, ensure_ascii=False) + ":" + canonical(v[k]) for k in sorted(v)) + "}"
    raise TypeError(type(v))


# ---- decoding (spec §4) -------------------------------------------------------------------------

def flag(value, key):
    """A PI flag: a JSON boolean or the string "true" / "false"; absent is false."""
    f = value.get(key, False)
    if isinstance(f, str):
        return f.lower() == "true"
    return f is True


def errors_of(v):
    if isinstance(v, dict) and isinstance(v.get("Errors"), list):
        msgs = []
        for e in v["Errors"]:
            if isinstance(e, str):
                msgs.append(e)
            elif isinstance(e, dict):
                # Per-field errors: {"FieldName": .., "Message": [..]}
                m = e.get("Message")
                msgs.extend(m if isinstance(m, list) else [str(m)])
        return "; ".join(msgs) if msgs else "error"
    return None


def is_digital(v):
    return isinstance(v, dict) and "Name" in v and "Value" in v and "Errors" not in v


def to_datatype(v, datatype):
    """(value, None) or (None, error)."""
    if is_digital(v):
        if datatype == "string":
            name = v["Name"]
            return (name, None) if isinstance(name, str) else (None, "unsupported PI value")
        v = v["Value"]
    if isinstance(v, bool):
        if datatype == "bool":
            return v, None
        if datatype == "string":
            return ("true" if v else "false"), None
        return None, f"expected {datatype}, got a boolean"
    if isinstance(v, (int, float)):
        n = float(v)
        if datatype == "string":
            return number_text(v), None
        if datatype == "bool":
            if n == 0:
                return False, None
            if n == 1:
                return True, None
            return None, f"{number_text(v)} does not fit bool"
        if datatype == "float64":
            return n, None
        if datatype == "float32":
            try:
                f = struct.unpack("<f", struct.pack("<f", n))[0]
            except OverflowError:
                return None, f"{number_text(v)} does not fit float32"
            return f, None
        lo, hi = INT_RANGES[datatype]
        if not math.isfinite(n) or n != math.floor(n) or n < lo or n > hi:
            return None, f"{number_text(v)} does not fit {datatype}"
        return (int(v) if isinstance(v, int) else int(n)), None
    if isinstance(v, str):
        if datatype == "string":
            return v, None
        return None, f"expected {datatype}, got a string"
    return None, "unsupported PI value"


def decode(pi, datatype, mode):
    v = pi.get("Value")
    out = {}
    err = errors_of(v) or errors_of(pi)
    if err is None and is_digital(v) and v.get("IsSystem") is True:
        err = v["Name"]
    if err is None and "Good" in pi and not flag(pi, "Good"):
        err = v["Name"] if is_digital(v) and isinstance(v["Name"], str) else "not good"
    if err is None and v is None:
        err = "unsupported PI value"
    value = None
    if err is None and mode == "typed":
        value, err = to_datatype(v, datatype)
    if err is None:
        out["quality"] = "good"
        if mode == "typed":
            out["value"] = value
    else:
        out["quality"] = "bad"
        out["error"] = err
    if mode == "raw":
        out["raw"] = canonical(v)
    flags = [name for key, name in (("Questionable", "questionable"), ("Substituted", "substituted"),
                                    ("Annotated", "annotated")) if flag(pi, key)]
    if flags:
        out["flags"] = flags
    return out


# ---- the vectors --------------------------------------------------------------------------------

TS = "2026-11-02T08:00:00Z"


def pv(value, **flags):
    d = {"Timestamp": TS, "Value": value, "UnitsAbbreviation": "", "Good": True,
         "Questionable": False, "Substituted": False, "Annotated": False}
    d.update(flags)
    return d


RUNNING = {"Name": "Running", "Value": 1, "IsSystem": False}
STOPPED = {"Name": "Stopped", "Value": 0, "IsSystem": False}
IO_TIMEOUT = {"Name": "I/O Timeout", "Value": 246, "IsSystem": True}
NO_DATA = {"Name": "No Data", "Value": 248, "IsSystem": True}
PT_CREATED = {"Name": "Pt Created", "Value": 253, "IsSystem": True}
SHUTDOWN = {"Name": "Shutdown", "Value": 254, "IsSystem": True}

INPUTS = [
    # name, point_type, datatype, mode, value
    ("Float64 as float64", "Float64", "float64", "typed", pv(81.5)),
    ("Float32 rounds to float32", "Float32", "float32", "typed", pv(0.1)),
    ("Float16 as float32", "Float16", "float32", "typed", pv(12.25)),
    ("Float64 integral value as int32", "Float64", "int32", "typed", pv(42.0)),
    ("Float64 fraction does not fit int32", "Float64", "int32", "typed", pv(42.5)),
    ("Float64 beyond float32 range", "Float64", "float32", "typed", pv(1e300)),
    ("Int16 as int16", "Int16", "int16", "typed", pv(-32768)),
    ("Int16 out of int8 range", "Int16", "int8", "typed", pv(200)),
    ("Int32 as int32", "Int32", "int32", "typed", pv(2147483647)),
    ("Int32 negative as uint32", "Int32", "uint32", "typed", pv(-1)),
    ("Int32 as uint8", "Int32", "uint8", "typed", pv(255)),
    ("Int32 as uint16", "Int32", "uint16", "typed", pv(65535)),
    ("Int32 as int64", "Int32", "int64", "typed", pv(-5)),
    ("Int32 as uint64", "Int32", "uint64", "typed", pv(7)),
    ("Int32 as float64", "Int32", "float64", "typed", pv(100)),
    ("number as string", "Float64", "string", "typed", pv(2.5)),
    ("integral number as string", "Float64", "string", "typed", pv(3.0)),
    ("number 0 as bool", "Int32", "bool", "typed", pv(0)),
    ("number 1 as bool", "Int32", "bool", "typed", pv(1)),
    ("number 2 does not fit bool", "Int32", "bool", "typed", pv(2)),
    ("String as string", "String", "string", "typed", pv("batch 42 ok")),
    ("String as float64 is bad", "String", "float64", "typed", pv("abc")),
    ("Timestamp point as string", "Timestamp", "string", "typed", pv("2026-11-01T23:00:00Z")),
    ("boolean value as bool", "Int32", "bool", "typed", pv(True)),
    ("boolean value as string", "Int32", "string", "typed", pv(False)),
    ("boolean value as int32 is bad", "Int32", "int32", "typed", pv(True)),
    ("Digital name", "Digital", "string", "typed", pv(RUNNING)),
    ("Digital code", "Digital", "int32", "typed", pv(RUNNING)),
    ("Digital code 0 as bool", "Digital", "bool", "typed", pv(STOPPED)),
    ("Digital code as float64", "Digital", "float64", "typed", pv(STOPPED)),
    ("system state I/O Timeout", "Digital", "string", "typed", pv(IO_TIMEOUT, Good=False)),
    ("system state on a Float32 point", "Float32", "float32", "typed", pv(PT_CREATED, Good=False)),
    ("system state Shutdown with Good true", "Float64", "float64", "typed", pv(SHUTDOWN)),
    ("deleted event (No Data)", "Float64", "float64", "typed", pv(NO_DATA, Good=False)),
    ("Good false on a number", "Float64", "float64", "typed", pv(3.5, Good=False)),
    ("Questionable stays good", "Float64", "float64", "typed", pv(1.5, Questionable=True)),
    ("Substituted and Annotated stay good", "Float64", "float64", "typed",
     pv(1.5, Substituted=True, Annotated=True)),
    ("flags as strings", "Float64", "float64", "typed",
     pv(9.0, Good="true", Questionable="true", Substituted="false", Annotated="false")),
    ("Good as the string false", "Float64", "float64", "typed", pv(9.0, Good="false")),
    ("flags absent", "Float64", "float64", "typed", {"Timestamp": TS, "Value": 4.25}),
    ("value with Errors", "Float64", "float64", "typed",
     pv({"Errors": ["[-11059] No Good Data For Calculation"]}, Good=False)),
    ("null value with field Errors", "Float64", "float64", "typed",
     dict(pv(None, Good=False), Errors=[{"FieldName": "Value", "Message": ["Point not found", "check the path"]}])),
    ("array value is unsupported", "Float64", "float64", "typed", pv([1, 2])),
    ("raw number", "Float64", "float64", "raw", pv(81.5)),
    ("raw integral number", "Int32", "int32", "raw", pv(7.0)),
    ("raw string", "String", "string", "raw", pv("batch \"42\"")),
    ("raw digital keys sorted", "Digital", "int32", "raw", pv(RUNNING)),
    ("raw system state is bad", "Digital", "int32", "raw", pv(IO_TIMEOUT, Good=False)),
    ("raw questionable", "Float64", "float64", "raw", pv(-0.5, Questionable=True)),
]


def main():
    vectors = []
    for name, point_type, datatype, mode, value in INPUTS:
        vectors.append({
            "name": name,
            "point_type": point_type,
            "datatype": datatype,
            "mode": mode,
            "value": value,
            "expect": decode(value, datatype, mode),
        })
    doc = {
        "description": "AVEVA PI value decoding (doc/connectors/aveva-pi-connector-spec.md §4). "
                       "Generated by connectors/aveva-pi/conformance/tools/gen_decode_vectors.py; do not edit.",
        "vectors": vectors,
    }
    json.dump(doc, sys.stdout, indent=2, ensure_ascii=False)
    sys.stdout.write("\n")


if __name__ == "__main__":
    main()
