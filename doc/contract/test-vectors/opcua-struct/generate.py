#!/usr/bin/env python3
"""Generate vectors.json for OPC UA structured values (see README.md).

The bodies are encoded by asyncua, an implementation independent of both connectors: each type
below is turned into a real `ua.StructureDefinition`, asyncua generates and registers the class
from it (as it does for a server's DataTypeDefinition), and its binary encoder writes the body.
The expected values are written by hand in the cases, never computed by the decoder under test.

Run it with the same Python and asyncua as the OPC UA simulator image (connectors/opcua/sim):

    uv run --python 3.12 --with 'asyncua~=1.1' doc/contract/test-vectors/opcua-struct/generate.py

asyncua 1.1 cannot encode on Python 3.14: with lazily evaluated annotations, fields such as
DataValue's `StatusCode_` resolve to the class's own property instead of the type.
"""

import json
import pathlib
import struct
import uuid
from datetime import datetime, timedelta, timezone

from asyncua import ua
from asyncua.common.structures104 import make_enum_code, make_structure_code
from asyncua.ua.ua_binary import Primitives, struct_to_binary, to_binary

HERE = pathlib.Path(__file__).resolve().parent
NS = 2  # namespace index of every custom type below
NAMESPACES = ["http://opcfoundation.org/UA/", "urn:tedge-dot:sim", "urn:acme:types"]

# -- type definitions -------------------------------------------------------------------------
# The vector form of a DataTypeDefinition. A field's `type` is a built-in type name (Part 6
# §5.1.2) or the name of another entry. `array` is ValueRank 1, `optional` is IsOptional.
TYPES = {
    "Mode": {"enum": ["Stopped", "Running", "Fault"]},
    "Motor": {"structure_type": "Structure", "fields": [
        {"name": "Current", "type": "Float"},
        {"name": "Temp", "type": "Float"},
    ]},
    "PumpStatus": {"structure_type": "Structure", "fields": [
        {"name": "Running", "type": "Boolean"},
        {"name": "Speed", "type": "Double"},
        {"name": "Motor", "type": "Motor"},
        {"name": "Label", "type": "String"},
        {"name": "Mode", "type": "Mode"},
    ]},
    "Batch": {"structure_type": "Structure", "fields": [
        {"name": "Label", "type": "String"},
        {"name": "Samples", "type": "Double", "array": True},
        {"name": "Count", "type": "UInt32"},
    ]},
    "Item": {"structure_type": "Structure", "fields": [
        {"name": "Name", "type": "String"},
        {"name": "Value", "type": "Double"},
    ]},
    "Order": {"structure_type": "Structure", "fields": [
        {"name": "Items", "type": "Item", "array": True},
        {"name": "Total", "type": "Double"},
    ]},
    "Note": {"structure_type": "StructureWithOptionalFields", "fields": [
        {"name": "Id", "type": "UInt16"},
        {"name": "Comment", "type": "String", "optional": True},
        {"name": "Level", "type": "Int32", "optional": True},
        {"name": "Tail", "type": "Byte"},
    ]},
    "Choice": {"structure_type": "Union", "fields": [
        {"name": "Name", "type": "String"},
        {"name": "Count", "type": "UInt32"},
    ]},
    # One field of every built-in type the walker has to step over, then a marker.
    "DiagHolder": {"structure_type": "Structure", "fields": [
        {"name": "Diag", "type": "DiagnosticInfo"},
        {"name": "Marker", "type": "UInt32"},
    ]},
    "Skippers": {"structure_type": "Structure", "fields": [
        {"name": "Flag", "type": "Boolean"},
        {"name": "I8", "type": "SByte"},
        {"name": "U64", "type": "UInt64"},
        {"name": "Text", "type": "String"},
        {"name": "Stamp", "type": "DateTime"},
        {"name": "Id", "type": "Guid"},
        {"name": "Blob", "type": "ByteString"},
        {"name": "Xml", "type": "XmlElement"},
        {"name": "Node", "type": "NodeId"},
        {"name": "XNode", "type": "ExpandedNodeId"},
        {"name": "Status", "type": "StatusCode"},
        {"name": "QName", "type": "QualifiedName"},
        {"name": "LText", "type": "LocalizedText"},
        {"name": "Ext", "type": "ExtensionObject"},
        {"name": "DValue", "type": "DataValue"},
        {"name": "Var", "type": "Variant"},
        {"name": "Diag", "type": "DiagnosticInfo"},
        {"name": "Nodes", "type": "NodeId", "array": True},
        {"name": "Texts", "type": "LocalizedText", "array": True},
        {"name": "Vars", "type": "Variant", "array": True},
        {"name": "Marker", "type": "UInt32"},
    ]},
    # One field of every additional built-in type that a point can select (design D8).
    "Builtins": {"structure_type": "Structure", "fields": [
        {"name": "Stamp", "type": "DateTime"},
        {"name": "LText", "type": "LocalizedText"},
        {"name": "Status", "type": "StatusCode"},
        {"name": "Id", "type": "Guid"},
        {"name": "Node", "type": "NodeId"},
        {"name": "XNode", "type": "ExpandedNodeId"},
        {"name": "QName", "type": "QualifiedName"},
        {"name": "Blob", "type": "ByteString"},
        {"name": "Stamps", "type": "DateTime", "array": True},
    ]},
}

BUILTIN_IDS = {
    "Boolean": 1, "SByte": 2, "Byte": 3, "Int16": 4, "UInt16": 5, "Int32": 6, "UInt32": 7,
    "Int64": 8, "UInt64": 9, "Float": 10, "Double": 11, "String": 12, "DateTime": 13,
    "Guid": 14, "ByteString": 15, "XmlElement": 16, "NodeId": 17, "ExpandedNodeId": 18,
    "StatusCode": 19, "QualifiedName": 20, "LocalizedText": 21, "ExtensionObject": 22,
    "DataValue": 23, "Variant": 24, "DiagnosticInfo": 25,
}


def type_ids(name):
    k = list(TYPES).index(name)
    return ua.NodeId(1000 + k, NS), ua.NodeId(2000 + k, NS)


def field_type_id(t):
    return ua.NodeId(BUILTIN_IDS[t], 0) if t in BUILTIN_IDS else type_ids(t)[0]


def register_types():
    classes = {}
    for name, t in TYPES.items():
        data_type, encoding = type_ids(name)
        env = {"ua": ua, "datetime": datetime, "timezone": timezone, "uuid": uuid}
        exec("from dataclasses import dataclass, field\nimport typing\nfrom enum import IntEnum, IntFlag", env)
        if "enum" in t:
            edef = ua.EnumDefinition()
            for i, member in enumerate(t["enum"]):
                f = ua.EnumField()
                f.Name, f.Value = member, i
                edef.Fields.append(f)
            exec(make_enum_code(name, edef, False), env)
            ua.register_enum(name, data_type, env[name])
        else:
            sdef = ua.StructureDefinition()
            sdef.StructureType = getattr(ua.StructureType, t["structure_type"])
            sdef.DefaultEncodingId = encoding
            for fd in t["fields"]:
                f = ua.StructureField()
                f.Name = fd["name"]
                f.DataType = field_type_id(fd["type"])
                f.ValueRank = 1 if fd.get("array") else -1
                f.IsOptional = bool(fd.get("optional"))
                sdef.Fields.append(f)
            exec(make_structure_code(data_type, name, sdef), env)
            ua.register_extension_object(name, encoding, env[name], data_type)
        classes[name] = env[name]
    return classes


C = register_types()


def make(name, **fields):
    """Build a structure value. A union takes exactly one field (or none)."""
    obj = C[name]()
    for k, v in fields.items():
        setattr(obj, k, v)
    return obj


def body(obj):
    return struct_to_binary(obj).hex()


# -- built-in sample values -------------------------------------------------------------------
STAMP = datetime(2026, 10, 6, 8, 15, 30, 250000, tzinfo=timezone.utc)
GUID = uuid.UUID("72962b91-fa75-4ae6-8d28-b404dc7daf63")


def skippers(marker, variant=0):
    """A Skippers value; `variant` switches every field to a different encoding."""
    if variant == 0:
        return make(
            "Skippers", Flag=True, I8=-5, U64=2**63 + 1, Text="äbc", Stamp=STAMP, Id=GUID,
            Blob=b"\x00\x01\x02", Xml=ua.XmlElement("<a/>"),
            Node=ua.NodeId(5, 0),  # two-byte encoding
            XNode=ua.ExpandedNodeId(Identifier="x", NamespaceUri="urn:other", ServerIndex=3),
            Status=ua.StatusCode(0x80340000), QName=ua.QualifiedName("Speed", 2),
            LText=ua.LocalizedText("Betrieb", "de-DE"),
            Ext=make("Motor", Current=1.0, Temp=2.0),
            DValue=ua.DataValue(ua.Variant(1.5, ua.VariantType.Double), ua.StatusCode(0),
                                SourceTimestamp=STAMP, ServerTimestamp=STAMP,
                                SourcePicoseconds=7, ServerPicoseconds=9),
            Var=ua.Variant([[1, 2], [3, 4]], ua.VariantType.Int32, Dimensions=[2, 2]),
            Diag=ua.DiagnosticInfo(SymbolicId=1, NamespaceURI=2, Locale=3, LocalizedText=4,
                                   AdditionalInfo="more", InnerStatusCode=ua.StatusCode(0x80010000)),
            Nodes=[ua.NodeId(300, 0), ua.NodeId(70000, 1), ua.NodeId("s", 2), ua.NodeId(GUID, 3),
                   ua.NodeId(b"\xff\x00", 4, ua.NodeIdType.ByteString)],
            Texts=[ua.LocalizedText("a"), ua.LocalizedText(None, "en"), ua.LocalizedText()],
            Vars=[ua.Variant("s"), ua.Variant([1.0, 2.0], ua.VariantType.Double),
                  ua.Variant(make("Item", Name="n", Value=1.0)), ua.Variant(None),
                  ua.Variant(ua.Variant(3, ua.VariantType.UInt16))],
            Marker=marker,
        )
    # Null and empty encodings everywhere.
    return make(
        "Skippers", Flag=False, I8=0, U64=0, Text=None, Stamp=datetime(1601, 1, 1, tzinfo=timezone.utc),
        Id=uuid.UUID(int=0), Blob=None, Xml=ua.XmlElement(None), Node=ua.NodeId(70000, 300),
        XNode=ua.ExpandedNodeId(7, 0), Status=ua.StatusCode(0), QName=ua.QualifiedName(),
        LText=ua.LocalizedText(), Ext=None, DValue=ua.DataValue(), Var=ua.Variant(None),
        Diag=ua.DiagnosticInfo(), Nodes=None, Texts=[], Vars=None, Marker=marker,
    )


# -- the vectors ------------------------------------------------------------------------------
def num(x): return {"num": x}
def text(s): return {"str": s}
def flag(b): return {"bool": b}
def hexv(h): return {"hex": h}


PUMP = make("PumpStatus", Running=True, Speed=1450.0, Motor=make("Motor", Current=3.25, Temp=41.5),
            Label="P1", Mode=C["Mode"].Running)
BATCH = make("Batch", Label="lot-7", Samples=[1.0, 2.0, 3.0, 4.5], Count=17)
ORDER = make("Order", Items=[make("Item", Name="first", Value=1.5), make("Item", Name="second", Value=7.0)],
             Total=8.5)
BUILTINS = make("Builtins", Stamp=STAMP, LText=ua.LocalizedText("Betrieb", "de-DE"),
                Status=ua.StatusCode(0x80340000), Id=GUID, Node=ua.NodeId(1001, 2),
                XNode=ua.ExpandedNodeId(Identifier="x", NamespaceUri="urn:other", ServerIndex=3),
                QName=ua.QualifiedName("Speed", 2), Blob=b"\xde\xad\xbe\xef",
                Stamps=[datetime(1970, 1, 1, tzinfo=timezone.utc), STAMP])

STAMP_TICKS = 134357481302500000  # STAMP as 100 ns ticks since 1601-01-01
STAMP_MS = 1791274530250

READ = [
    # Fields
    dict(name="top-level Double field", root="PumpStatus", body=body(PUMP), path="Speed",
         datatype="float64", out=num(1450.0), raw="4096a80000000000"),
    dict(name="top-level Boolean field", root="PumpStatus", body=body(PUMP), path="Running",
         datatype="bool", out=flag(True), raw="01"),
    dict(name="nested field", root="PumpStatus", body=body(PUMP), path="Motor.Current",
         datatype="float32", out=num(3.25), raw="40500000"),
    dict(name="nested field after a nested field", root="PumpStatus", body=body(PUMP), path="Motor.Temp",
         datatype="float32", out=num(41.5), raw="42260000"),
    dict(name="String field after a nested structure", root="PumpStatus", body=body(PUMP), path="Label",
         datatype="string", out=text("P1"), raw="5031"),
    dict(name="Enumeration field as int32", root="PumpStatus", body=body(PUMP), path="Mode",
         datatype="int32", out=num(1), raw="00000001"),
    dict(name="field after a String and an array", root="Batch", body=body(BATCH), path="Count",
         datatype="uint32", out=num(17), raw="00000011"),
    dict(name="field after a null String and a null array", root="Batch",
         body=body(make("Batch", Label=None, Samples=None, Count=9)), path="Count",
         datatype="uint32", out=num(9), raw="00000009"),
    dict(name="every built-in type skipped", root="Skippers", body=body(skippers(0xC0FFEE)), path="Marker",
         datatype="uint32", out=num(0xC0FFEE), raw="00c0ffee"),
    dict(name="every built-in type skipped, null and alternative encodings", root="Skippers",
         body=body(skippers(42, variant=1)), path="Marker", datatype="uint32", out=num(42), raw="0000002a"),
    # asyncua cannot encode a DiagnosticInfo nested in another, so this body is assembled by hand.
    dict(name="nested DiagnosticInfo skipped", root="DiagHolder",
         body="41" + "01000000"            # mask: SymbolicId + InnerDiagnosticInfo; SymbolicId 1
              + "50" + "02000000" + "696e"  # inner mask: AdditionalInfo + InnerDiagnosticInfo; "in"
              + "21" + "07000000" + "0000ac80"  # innermost: SymbolicId 7 + InnerStatusCode
              + "05000000",                 # Marker
         path="Marker", datatype="uint32", out=num(5), raw="00000005"),
    # Array elements
    dict(name="element of an array field", root="Batch", body=body(BATCH), path="Samples[3]",
         datatype="float64", out=num(4.5), raw="4012000000000000"),
    dict(name="first element of an array field", root="Batch", body=body(BATCH), path="Samples[0]",
         datatype="float64", out=num(1.0), raw="3ff0000000000000"),
    dict(name="field of a structure element", root="Order", body=body(ORDER), path="Items[1].Value",
         datatype="float64", out=num(7.0), raw="401c000000000000"),
    dict(name="String field of a structure element", root="Order", body=body(ORDER), path="Items[1].Name",
         datatype="string", out=text("second"), raw="7365636f6e64"),
    dict(name="field after an array of structures", root="Order", body=body(ORDER), path="Total",
         datatype="float64", out=num(8.5), raw="4021000000000000"),
    dict(name="index out of range", root="Batch", body=body(make("Batch", Label="", Samples=[1.0, 2.0], Count=0)),
         path="Samples[5]", datatype="float64", error='index 5 out of range for "Samples" (length 2)'),
    dict(name="index into a null array", root="Batch", body=body(make("Batch", Label="", Samples=None, Count=0)),
         path="Samples[0]", datatype="float64", error='index 0 out of range for "Samples" (length 0)'),
    dict(name="array without an index", root="Batch", body=body(BATCH), path="Samples", datatype="float64",
         error='field "Samples" is an array of Double; select one element, e.g. "Samples[0]"'),
    dict(name="index on a scalar", root="Batch", body=body(BATCH), path="Count[0]", datatype="uint32",
         error='field "Count" is not an array'),
    # Optional fields and unions
    dict(name="present optional field", root="Note", body=body(make("Note", Id=1, Comment="hi", Level=None, Tail=7)),
         path="Comment", datatype="string", out=text("hi"), raw="6869"),
    dict(name="absent optional field", root="Note", body=body(make("Note", Id=1, Comment=None, Level=-3, Tail=7)),
         path="Comment", datatype="string", error='optional field "Comment" is absent'),
    dict(name="field after absent optional fields", root="Note",
         body=body(make("Note", Id=1, Comment=None, Level=None, Tail=7)),
         path="Tail", datatype="uint8", out=num(7), raw="07"),
    dict(name="present optional after absent optional", root="Note",
         body=body(make("Note", Id=1, Comment=None, Level=-3, Tail=7)),
         path="Level", datatype="int32", out=num(-3), raw="fffffffd"),
    dict(name="active union member", root="Choice", body=body(make("Choice", Count=5)), path="Count",
         datatype="uint32", out=num(5), raw="00000005"),
    dict(name="inactive union member", root="Choice", body=body(make("Choice", Name="x")), path="Count",
         datatype="uint32", error='union field "Count" is not set (active: "Name")'),
    dict(name="empty union", root="Choice", body=body(make("Choice")), path="Name",
         datatype="string", error='union field "Name" is not set (active: none)'),
    # Path and type errors
    dict(name="unknown field", root="PumpStatus", body=body(PUMP), path="Sped", datatype="float64",
         error='field "Sped" not found in PumpStatus (fields: Running, Speed, Motor, Label, Mode)'),
    dict(name="unknown nested field", root="PumpStatus", body=body(PUMP), path="Motor.Volts", datatype="float32",
         error='field "Volts" not found in Motor (fields: Current, Temp)'),
    dict(name="structure selected", root="PumpStatus", body=body(PUMP), path="Motor", datatype="float64",
         error='field "Motor" is a structure (Motor); select one of its fields, e.g. "Motor.Current"'),
    dict(name="descending into a scalar", root="PumpStatus", body=body(PUMP), path="Speed.Value",
         datatype="float64", error='field "Speed" is Double, not a structure'),
    dict(name="declared type mismatch", root="PumpStatus", body=body(PUMP), path="Speed", datatype="int32",
         error='field "Speed" is Double, point declares int32 (accepted: float64)'),
    dict(name="non-selectable built-in type", root="Skippers", body=body(skippers(1)), path="Var",
         datatype="string", error='field "Var" is Variant, which a point cannot read'),
    # Malformed bodies
    dict(name="truncated body", root="PumpStatus", body=body(PUMP)[:-4], path="Mode", datatype="int32",
         error='structure body truncated while reading "Mode"'),
    dict(name="truncated in a skipped field", root="Batch", body=body(BATCH)[:30], path="Count",
         datatype="uint32", error='structure body truncated while reading "Samples"'),
    dict(name="length prefix beyond the body", root="Batch",
         body=struct.pack("<i", 1000).hex() + "6c6f74", path="Count", datatype="uint32",
         error='structure body truncated while reading "Label"'),
    dict(name="negative length prefix", root="Batch", body=struct.pack("<i", -5).hex(), path="Count",
         datatype="uint32", error='invalid length -5 while reading "Label"'),
    dict(name="trailing bytes are ignored", root="Motor", body=body(make("Motor", Current=1.0, Temp=2.0)) + "ff",
         path="Temp", datatype="float32", out=num(2.0), raw="40000000"),
    # Additional built-in types (design D8)
    dict(name="DateTime as string", root="Builtins", body=body(BUILTINS), path="Stamp", datatype="string",
         out=text("2026-10-06T08:15:30.25Z"), raw=b"2026-10-06T08:15:30.25Z".hex()),
    dict(name="DateTime as int64", root="Builtins", body=body(BUILTINS), path="Stamp", datatype="int64",
         out=num(STAMP_MS), raw=struct.pack(">q", STAMP_TICKS).hex()),
    dict(name="DateTime array element", root="Builtins", body=body(BUILTINS), path="Stamps[0]", datatype="string",
         out=text("1970-01-01T00:00:00Z"), raw=b"1970-01-01T00:00:00Z".hex()),
    dict(name="DateTime not accepted as float64", root="Builtins", body=body(BUILTINS), path="Stamp",
         datatype="float64", error='field "Stamp" is DateTime, point declares float64 (accepted: string, int64)'),
    dict(name="LocalizedText", root="Builtins", body=body(BUILTINS), path="LText", datatype="string",
         out=text("Betrieb"), raw=b"Betrieb".hex()),
    dict(name="StatusCode as uint32", root="Builtins", body=body(BUILTINS), path="Status", datatype="uint32",
         out=num(0x80340000), raw="80340000"),
    dict(name="StatusCode as string", root="Builtins", body=body(BUILTINS), path="Status", datatype="string",
         out=text("BadNodeIdUnknown"), raw=b"BadNodeIdUnknown".hex()),
    dict(name="Guid", root="Builtins", body=body(BUILTINS), path="Id", datatype="string",
         out=text(str(GUID)), raw=str(GUID).encode().hex()),
    dict(name="NodeId with namespace URI", root="Builtins", body=body(BUILTINS), path="Node", datatype="string",
         out=text("nsu=urn:acme:types;i=1001"), raw=b"nsu=urn:acme:types;i=1001".hex()),
    dict(name="ExpandedNodeId with server index", root="Builtins", body=body(BUILTINS), path="XNode",
         datatype="string", out=text("svr=3;nsu=urn:other;s=x"), raw=b"svr=3;nsu=urn:other;s=x".hex()),
    dict(name="QualifiedName", root="Builtins", body=body(BUILTINS), path="QName", datatype="string",
         out=text("2:Speed"), raw=b"2:Speed".hex()),
    dict(name="ByteString as bytes", root="Builtins", body=body(BUILTINS), path="Blob", datatype="bytes",
         out=hexv("deadbeef"), raw="deadbeef"),
]

# Top-level values: `root` is a built-in type and `body` is that value's binary encoding, as
# the walker sees an array element or a field. The connectors convert their library's decoded
# value into the same form, so these also pin the rendering of a top-level variable.
def builtin(t, v):
    return to_binary(getattr(ua, t) if hasattr(ua, t) else getattr(ua.VariantType, t), v).hex()


def ticks(n):
    return struct.pack("<q", n).hex()


MAX_TICKS = 2650467743999999999  # 9999-12-31T23:59:59.9999999Z
UNIX_EPOCH_TICKS = 116444736000000000

READ += [
    dict(name="DateTime zero (not set)", root="DateTime", body=ticks(0), datatype="string",
         out=text("1601-01-01T00:00:00Z"), raw=b"1601-01-01T00:00:00Z".hex()),
    dict(name="DateTime zero as int64", root="DateTime", body=ticks(0), datatype="int64",
         out=num(-11644473600000), raw="0000000000000000"),
    dict(name="DateTime negative clamps to zero", root="DateTime", body=ticks(-5), datatype="string",
         out=text("1601-01-01T00:00:00Z"), raw=b"1601-01-01T00:00:00Z".hex()),
    dict(name="DateTime full precision", root="DateTime", body=ticks(UNIX_EPOCH_TICKS + 1234567), datatype="string",
         out=text("1970-01-01T00:00:00.1234567Z"), raw=b"1970-01-01T00:00:00.1234567Z".hex()),
    dict(name="DateTime int64 rounds towards negative infinity", root="DateTime",
         body=ticks(UNIX_EPOCH_TICKS - 5000), datatype="int64", out=num(-1),
         raw=struct.pack(">q", UNIX_EPOCH_TICKS - 5000).hex()),
    dict(name="DateTime beyond year 9999 clamps", root="DateTime", body=ticks(2**63 - 1), datatype="string",
         out=text("9999-12-31T23:59:59.9999999Z"), raw=b"9999-12-31T23:59:59.9999999Z".hex()),
    dict(name="DateTime maximum as int64", root="DateTime", body=ticks(2**63 - 1), datatype="int64",
         out=num((MAX_TICKS - UNIX_EPOCH_TICKS) // 10000), raw=struct.pack(">q", MAX_TICKS).hex()),
    dict(name="LocalizedText without text", root="LocalizedText", body=builtin("LocalizedText", ua.LocalizedText(None, "en")),
         datatype="string", out=text(""), raw=""),
    dict(name="null String", root="String", body=struct.pack("<i", -1).hex(), datatype="string", out=text(""), raw=""),
    dict(name="null ByteString", root="ByteString", body=struct.pack("<i", -1).hex(), datatype="bytes",
         out=hexv(""), raw=""),
    dict(name="StatusCode Good by name", root="StatusCode", body=builtin("StatusCode", ua.StatusCode(0)),
         datatype="string", out=text("Good"), raw=b"Good".hex()),
    dict(name="StatusCode without a name", root="StatusCode", body=struct.pack("<I", 0x80FF0000).hex(),
         datatype="string", out=text("0x80FF0000"), raw=b"0x80FF0000".hex()),
    dict(name="StatusCode with info bits has no name", root="StatusCode", body=struct.pack("<I", 0x80340400).hex(),
         datatype="string", out=text("0x80340400"), raw=b"0x80340400".hex()),
    dict(name="NodeId numeric in namespace 0", root="NodeId", body=builtin("NodeId", ua.NodeId(2253, 0)),
         datatype="string", out=text("i=2253"), raw=b"i=2253".hex()),
    dict(name="NodeId string", root="NodeId", body=builtin("NodeId", ua.NodeId("Pump1.Status", 1)),
         datatype="string", out=text("nsu=urn:tedge-dot:sim;s=Pump1.Status"),
         raw=b"nsu=urn:tedge-dot:sim;s=Pump1.Status".hex()),
    dict(name="NodeId guid", root="NodeId", body=builtin("NodeId", ua.NodeId(GUID, 2)),
         datatype="string", out=text(f"nsu=urn:acme:types;g={GUID}"), raw=f"nsu=urn:acme:types;g={GUID}".encode().hex()),
    dict(name="NodeId opaque", root="NodeId", body=builtin("NodeId", ua.NodeId(b"\xff\x00\x10", 2, ua.NodeIdType.ByteString)),
         datatype="string", out=text("nsu=urn:acme:types;b=/wAQ"), raw=b"nsu=urn:acme:types;b=/wAQ".hex()),
    dict(name="NodeId with unknown namespace index", root="NodeId", body=builtin("NodeId", ua.NodeId(7, 9)),
         datatype="string", out=text("ns=9;i=7"), raw=b"ns=9;i=7".hex()),
    dict(name="ExpandedNodeId local", root="ExpandedNodeId", body=builtin("ExpandedNodeId", ua.ExpandedNodeId(7, 2)),
         datatype="string", out=text("nsu=urn:acme:types;i=7"), raw=b"nsu=urn:acme:types;i=7".hex()),
    dict(name="QualifiedName in namespace 0", root="QualifiedName", body=builtin("QualifiedName", ua.QualifiedName("Name", 0)),
         datatype="string", out=text("Name"), raw=b"Name".hex()),
]

INVALID_PATHS = [
    {"path": "", "error": "field path is empty"},
    {"path": "Motor..Current", "error": 'field path "Motor..Current" has an empty segment'},
    {"path": "Samples[]", "error": 'field path "Samples[]" has an invalid index'},
    {"path": "Samples[-1]", "error": 'field path "Samples[-1]" has an invalid index'},
    {"path": "Samples[1", "error": 'field path "Samples[1" has an invalid index'},
    {"path": "Samples[1]x", "error": 'field path "Samples[1]x" has an invalid index'},
]


def check():
    """Cross-check the hand-written expectations against asyncua's own decoder."""
    import re
    from asyncua.common.utils import Buffer
    from asyncua.ua.ua_binary import struct_from_binary

    assert Primitives.DateTime.pack(STAMP) == struct.pack("<q", STAMP_TICKS)
    assert (STAMP_TICKS - UNIX_EPOCH_TICKS) // 10000 == STAMP_MS
    checked = 0
    for v in READ:
        # Only plain fields: the rendered built-in types are what the vectors define.
        if "out" not in v or v["root"] not in C or v["root"] == "Builtins":
            continue
        try:
            obj = struct_from_binary(C[v["root"]], Buffer(bytes.fromhex(v["body"])))
        except Exception:
            continue  # hand-assembled bodies asyncua cannot decode (nested DiagnosticInfo)
        for seg in v["path"].split("."):
            m = re.fullmatch(r"(\w+)(?:\[(\d+)\])?", seg)
            obj = getattr(obj, m.group(1))
            if m.group(2) is not None:
                obj = obj[int(m.group(2))]
        got = obj.value if hasattr(obj, "value") and not isinstance(obj, (str, bytes)) else obj
        want = next(iter(v["out"].values()))
        assert got == want, (v["name"], got, want)
        checked += 1
    print(f"cross-checked {checked} expectations against asyncua's decoder")


def main():
    check()
    out = {
        "namespaces": NAMESPACES,
        "types": TYPES,
        "read": READ,
        "invalid_paths": INVALID_PATHS,
    }
    (HERE / "vectors.json").write_text(json.dumps(out, indent=1, ensure_ascii=False) + "\n")
    print(f"wrote {len(READ)} read vectors")


if __name__ == "__main__":
    main()
