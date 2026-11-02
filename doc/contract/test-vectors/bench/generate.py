#!/usr/bin/env python3
"""Write the `bench` module value vectors (see README.md) to stdout.

An implementation of the algorithm independent of the Rust and C modules: both builds' unit
tests check their output against what this script produces.
"""
import json
import math
import struct
import sys

MASK = (1 << 64) - 1
COUNT = 1000


def fnv1a64(data: bytes) -> int:
    h = 0xCBF29CE484222325
    for b in data:
        h ^= b
        h = (h * 0x100000001B3) & MASK
    return h


class Stream:
    def __init__(self, device: str, point: str):
        seed = fnv1a64(device.encode() + b"\0" + point.encode())
        self.x = seed or 1

    def next(self) -> int:
        x = self.x
        x ^= x >> 12
        x ^= (x << 25) & MASK
        x ^= x >> 27
        self.x = x
        return (x * 0x2545F4914F6CDD1D) & MASK

    def draw(self) -> float:
        return float(self.next() >> 11) * (2.0 ** -53)


def round_half_away(v: float) -> int:
    a = abs(v)
    r = math.floor(a)
    if a - r >= 0.5:  # exact for doubles, unlike floor(a + 0.5)
        r += 1
    r = r if v >= 0 else -r
    return max(-(2 ** 31), min(2 ** 31 - 1, r))


def bits(v: float) -> str:
    return "0x%016x" % struct.unpack("<Q", struct.pack("<d", v))[0]


def values(case):
    s = Stream(case["device"], case["point"])
    model, step, change = case["model"], case["step"], case["change"]
    acc, changes, out = 0.0, 0, []
    for _ in range(COUNT):
        u1 = s.draw()
        if u1 < change and model != "constant":
            changes += 1
            if model == "walk":
                u2 = s.draw()
                acc = acc + ((2.0 * u2 - 1.0) * step)
            elif model == "counter":
                acc = acc + 1.0
        dt = case["datatype"]
        if dt == "float64":
            out.append(bits(acc))
        elif dt == "int32":
            out.append(round_half_away(acc))
        elif dt == "bool":
            out.append(changes % 2 == 1)
        else:
            out.append(str(changes))
    return out


def case(name, device, point, model="walk", datatype="float64", step=1.0, change=1.0):
    return dict(name=name, device=device, point=point, model=model, datatype=datatype,
                step=step, change=change)


CASES = [
    case("walk float64, defaults", "plc0", "p0"),
    case("walk float64, change 0.2, step 0.5", "plc0", "p1", step=0.5, change=0.2),
    case("walk float64, change 0", "plc0", "p2", change=0.0),
    case("walk float64, large step", "plc7", "temperature", step=1000.0),
    case("walk int32, step 3.7", "plc1", "level", datatype="int32", step=3.7),
    case("walk int32, half-step rounding", "plc1", "half", datatype="int32", step=0.5, change=0.5),
    case("walk bool, change 0.2", "plc2", "valve", datatype="bool", change=0.2),
    case("walk string, change 0.2", "plc2", "state", datatype="string", change=0.2),
    case("counter float64", "plc3", "count", model="counter"),
    case("counter int32, change 0.5", "plc3", "pulses", model="counter", datatype="int32", change=0.5),
    case("counter string, change 1.0", "plc3", "batch", model="counter", datatype="string"),
    case("constant float64", "plc4", "fixed", model="constant"),
    case("constant bool", "plc4", "flag", model="constant", datatype="bool"),
    case("non-ASCII ids", "plc-ä", "温度/µ", step=2.0),
]

if __name__ == "__main__":
    # One case per line keeps the file small and its diffs readable.
    lines = [json.dumps(dict(c, values=values(c)), ensure_ascii=False) for c in CASES]
    sys.stdout.write('{"cases": [\n' + ",\n".join(lines) + "\n]}\n")
