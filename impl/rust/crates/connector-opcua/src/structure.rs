//! Reading one value out of an OPC UA structure, an array, or one of the built-in types that
//! have no SDK primitive of their own (openspec change `opcua-custom-datatypes`, design D3, D7
//! and D8).
//!
//! The connector reads a structure's layout from the server (`DataTypeDefinition`) and turns it
//! into a [`TypeSet`]. A point's `field` path is checked against it once, giving a [`Plan`].
//! Each value is then decoded by walking the binary body (OPC UA Part 6 §5.2) with that plan:
//! every field before the selected one is skipped by its encoded length, so no type knowledge
//! is compiled in. Everything here is pure and shared with the C connector through the vectors
//! in `doc/contract/test-vectors/opcua-struct/`.

use std::collections::HashMap;
use tedge_dot_sdk::{DataType, Value};

/// Ticks (100 ns) between the OPC UA epoch (1601-01-01) and the Unix epoch.
pub const UNIX_EPOCH_TICKS: i64 = 116_444_736_000_000_000;
/// 9999-12-31T23:59:59.9999999Z, where Part 6 clamps every later DateTime.
pub const MAX_TICKS: i64 = 2_650_467_743_999_999_999;
/// How deep nested structures, Variants and DiagnosticInfos may go before a body is refused.
const MAX_DEPTH: usize = 32;

/// The OPC UA built-in types (Part 6 §5.1.2), numbered as on the wire.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Builtin {
    Boolean = 1,
    SByte,
    Byte,
    Int16,
    UInt16,
    Int32,
    UInt32,
    Int64,
    UInt64,
    Float,
    Double,
    String,
    DateTime,
    Guid,
    ByteString,
    XmlElement,
    NodeId,
    ExpandedNodeId,
    StatusCode,
    QualifiedName,
    LocalizedText,
    ExtensionObject,
    DataValue,
    Variant,
    DiagnosticInfo,
}

const BUILTINS: [Builtin; 25] = [
    Builtin::Boolean,
    Builtin::SByte,
    Builtin::Byte,
    Builtin::Int16,
    Builtin::UInt16,
    Builtin::Int32,
    Builtin::UInt32,
    Builtin::Int64,
    Builtin::UInt64,
    Builtin::Float,
    Builtin::Double,
    Builtin::String,
    Builtin::DateTime,
    Builtin::Guid,
    Builtin::ByteString,
    Builtin::XmlElement,
    Builtin::NodeId,
    Builtin::ExpandedNodeId,
    Builtin::StatusCode,
    Builtin::QualifiedName,
    Builtin::LocalizedText,
    Builtin::ExtensionObject,
    Builtin::DataValue,
    Builtin::Variant,
    Builtin::DiagnosticInfo,
];

impl Builtin {
    /// The built-in type with this id (1–25), as in a Variant's encoding mask or a
    /// namespace-0 DataType node id.
    pub fn from_id(id: u32) -> Option<Builtin> {
        BUILTINS.get((id as usize).checked_sub(1)?).copied()
    }

    pub fn name(self) -> &'static str {
        match self {
            Builtin::Boolean => "Boolean",
            Builtin::SByte => "SByte",
            Builtin::Byte => "Byte",
            Builtin::Int16 => "Int16",
            Builtin::UInt16 => "UInt16",
            Builtin::Int32 => "Int32",
            Builtin::UInt32 => "UInt32",
            Builtin::Int64 => "Int64",
            Builtin::UInt64 => "UInt64",
            Builtin::Float => "Float",
            Builtin::Double => "Double",
            Builtin::String => "String",
            Builtin::DateTime => "DateTime",
            Builtin::Guid => "Guid",
            Builtin::ByteString => "ByteString",
            Builtin::XmlElement => "XmlElement",
            Builtin::NodeId => "NodeId",
            Builtin::ExpandedNodeId => "ExpandedNodeId",
            Builtin::StatusCode => "StatusCode",
            Builtin::QualifiedName => "QualifiedName",
            Builtin::LocalizedText => "LocalizedText",
            Builtin::ExtensionObject => "ExtensionObject",
            Builtin::DataValue => "DataValue",
            Builtin::Variant => "Variant",
            Builtin::DiagnosticInfo => "DiagnosticInfo",
        }
    }

    pub fn from_name(name: &str) -> Option<Builtin> {
        BUILTINS.iter().copied().find(|b| b.name() == name)
    }
}

/// The type of a structure field.
#[derive(Clone, Debug, PartialEq)]
pub enum FieldType {
    Builtin(Builtin),
    /// An Enumeration, encoded as Int32. Carries the type's display name.
    Enum(String),
    /// A structure, by its key in the [`TypeSet`].
    Struct(String),
}

#[derive(Clone, Debug, PartialEq)]
pub struct Field {
    pub name: String,
    pub ty: FieldType,
    /// ValueRank 1. Multi-dimensional fields are refused when the definition is resolved.
    pub array: bool,
    pub optional: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum StructureKind {
    Structure,
    OptionalFields,
    Union,
}

#[derive(Clone, Debug, PartialEq)]
pub struct StructDef {
    /// The type's display name (its browse name), used in messages.
    pub name: String,
    pub kind: StructureKind,
    pub fields: Vec<Field>,
}

/// Every structure definition a point's path may cross or skip, keyed by an id of the caller's
/// choosing (the connector uses the DataType's node id).
#[derive(Clone, Debug, Default)]
pub struct TypeSet {
    pub structs: HashMap<String, StructDef>,
}

impl TypeSet {
    fn get(&self, key: &str) -> Result<&StructDef, String> {
        self.structs
            .get(key)
            .ok_or_else(|| format!("no definition for structure type {key}"))
    }

    fn type_name(&self, ty: &FieldType) -> String {
        match ty {
            FieldType::Builtin(b) => b.name().to_string(),
            FieldType::Enum(name) => name.clone(),
            FieldType::Struct(key) => self.structs.get(key).map_or(key.clone(), |d| d.name.clone()),
        }
    }
}

/// One segment of a `field` path: a field name, optionally with an element index.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Segment {
    pub name: String,
    pub index: Option<u32>,
}

impl Segment {
    fn text(&self) -> String {
        match self.index {
            Some(i) => format!("{}[{i}]", self.name),
            None => self.name.clone(),
        }
    }
}

/// Parse a point's `field` path, e.g. `Motor.Current` or `Items[1].Value`.
pub fn parse_path(path: &str) -> Result<Vec<Segment>, String> {
    if path.is_empty() {
        return Err("field path is empty".into());
    }
    let mut out = Vec::new();
    for part in path.split('.') {
        let (name, index) = match part.find('[') {
            None => (part, None),
            Some(open) => {
                let rest = &part[open + 1..];
                let digits = rest.strip_suffix(']').filter(|d| {
                    !d.is_empty() && d.bytes().all(|b| b.is_ascii_digit())
                });
                let index = digits
                    .and_then(|d| d.parse::<u32>().ok())
                    .ok_or_else(|| format!("field path \"{path}\" has an invalid index"))?;
                (&part[..open], Some(index))
            }
        };
        if name.is_empty() {
            return Err(format!("field path \"{path}\" has an empty segment"));
        }
        if name.contains(']') {
            return Err(format!("field path \"{path}\" has an invalid index"));
        }
        out.push(Segment { name: name.to_string(), index });
    }
    Ok(out)
}

/// What a point finally reads: a built-in type or an Enumeration.
#[derive(Clone, Debug, PartialEq)]
pub enum Leaf {
    Builtin(Builtin),
    Enum(String),
}

impl Leaf {
    fn name(&self) -> &str {
        match self {
            Leaf::Builtin(b) => b.name(),
            Leaf::Enum(name) => name,
        }
    }
}

/// The datatypes a point may declare for a value of this type; empty when no point can read it.
pub fn accepted(leaf: &Leaf) -> &'static [DataType] {
    use DataType as D;
    match leaf {
        Leaf::Enum(_) => &[D::Int32],
        Leaf::Builtin(b) => match b {
            Builtin::Boolean => &[D::Bool],
            Builtin::SByte => &[D::Int8],
            Builtin::Byte => &[D::Uint8],
            Builtin::Int16 => &[D::Int16],
            Builtin::UInt16 => &[D::Uint16],
            Builtin::Int32 => &[D::Int32],
            Builtin::UInt32 => &[D::Uint32],
            Builtin::Int64 => &[D::Int64],
            Builtin::UInt64 => &[D::Uint64],
            Builtin::Float => &[D::Float32],
            Builtin::Double => &[D::Float64],
            Builtin::String => &[D::String],
            Builtin::DateTime => &[D::String, D::Int64],
            Builtin::StatusCode => &[D::Uint32, D::String],
            Builtin::LocalizedText
            | Builtin::Guid
            | Builtin::NodeId
            | Builtin::ExpandedNodeId
            | Builtin::QualifiedName => &[D::String],
            Builtin::ByteString => &[D::Bytes],
            Builtin::XmlElement
            | Builtin::ExtensionObject
            | Builtin::DataValue
            | Builtin::Variant
            | Builtin::DiagnosticInfo => &[],
        },
    }
}

pub fn datatype_name(dt: DataType) -> &'static str {
    match dt {
        DataType::Bool => "bool",
        DataType::Int8 => "int8",
        DataType::Uint8 => "uint8",
        DataType::Int16 => "int16",
        DataType::Uint16 => "uint16",
        DataType::Int32 => "int32",
        DataType::Uint32 => "uint32",
        DataType::Int64 => "int64",
        DataType::Uint64 => "uint64",
        DataType::Float32 => "float32",
        DataType::Float64 => "float64",
        DataType::String => "string",
        DataType::Bytes => "bytes",
    }
}

/// Check that a point declaring `dt` can read a value of type `leaf`. `subject` names the
/// value in the message: `field "Speed"`, or `value` for a top-level variable.
pub fn check_datatype(leaf: &Leaf, dt: DataType, subject: &str) -> Result<(), String> {
    let ok = accepted(leaf);
    if ok.is_empty() {
        return Err(format!("{subject} is {}, which a point cannot read", leaf.name()));
    }
    if ok.contains(&dt) {
        return Ok(());
    }
    let list: Vec<&str> = ok.iter().map(|d| datatype_name(*d)).collect();
    Err(format!(
        "{subject} is {}, point declares {} (accepted: {})",
        leaf.name(),
        datatype_name(dt),
        list.join(", ")
    ))
}

/// A point's path, checked against the definitions: which field to take at each level.
#[derive(Clone, Debug, PartialEq)]
pub struct Plan {
    steps: Vec<Step>,
    leaf: Leaf,
    datatype: DataType,
}

#[derive(Clone, Debug, PartialEq)]
struct Step {
    /// The structure this step reads from.
    key: String,
    /// The position of the selected field in it.
    field: usize,
    index: Option<u32>,
}

/// Check a path against the definitions and the point's declared datatype.
pub fn compile(types: &TypeSet, root: &str, path: &[Segment], dt: DataType) -> Result<Plan, String> {
    let mut key = root.to_string();
    let mut steps = Vec::new();
    for (i, seg) in path.iter().enumerate() {
        let def = types.get(&key)?;
        let pos = def.fields.iter().position(|f| f.name == seg.name).ok_or_else(|| {
            let names: Vec<&str> = def.fields.iter().map(|f| f.name.as_str()).collect();
            format!(
                "field \"{}\" not found in {} (fields: {})",
                seg.name,
                def.name,
                names.join(", ")
            )
        })?;
        let field = &def.fields[pos];
        let so_far: Vec<String> = path[..=i].iter().map(Segment::text).collect();
        let so_far = so_far.join(".");
        match (field.array, seg.index) {
            (false, Some(_)) => return Err(format!("field \"{}\" is not an array", field.name)),
            (true, None) => {
                return Err(format!(
                    "field \"{}\" is an array of {}; select one element, e.g. \"{so_far}[0]\"",
                    field.name,
                    types.type_name(&field.ty)
                ))
            }
            _ => {}
        }
        steps.push(Step { key: key.clone(), field: pos, index: seg.index });
        let last = i + 1 == path.len();
        match (&field.ty, last) {
            (FieldType::Struct(next), false) => key = next.clone(),
            (other, false) => {
                return Err(format!(
                    "field \"{}\" is {}, not a structure",
                    field.name,
                    types.type_name(other)
                ))
            }
            (FieldType::Struct(next), true) => {
                let inner = types.get(next)?;
                let first = inner.fields.first().map_or("", |f| f.name.as_str());
                return Err(format!(
                    "field \"{}\" is a structure ({}); select one of its fields, e.g. \"{so_far}.{first}\"",
                    field.name, inner.name
                ));
            }
            (FieldType::Builtin(b), true) => {
                let leaf = Leaf::Builtin(*b);
                check_datatype(&leaf, dt, &format!("field \"{}\"", field.name))?;
                return Ok(Plan { steps, leaf, datatype: dt });
            }
            (FieldType::Enum(name), true) => {
                let leaf = Leaf::Enum(name.clone());
                check_datatype(&leaf, dt, &format!("field \"{}\"", field.name))?;
                return Ok(Plan { steps, leaf, datatype: dt });
            }
        }
    }
    Err("field path is empty".into())
}

/// A decoded value of one of the built-in types a point can read.
#[derive(Clone, Debug, PartialEq)]
pub enum Scalar {
    Bool(bool),
    I8(i8),
    U8(u8),
    I16(i16),
    U16(u16),
    I32(i32),
    U32(u32),
    I64(i64),
    U64(u64),
    F32(f32),
    F64(f64),
    /// String, LocalizedText's text, or XmlElement; `None` is the null encoding.
    Text(Option<String>),
    /// 100 ns ticks since 1601-01-01, as on the wire.
    DateTime(i64),
    /// The 16 bytes as encoded: Data1–Data3 little-endian, Data4 as is.
    Guid([u8; 16]),
    ByteString(Option<Vec<u8>>),
    StatusCode(u32),
    NodeId(NodeId),
    ExpandedNodeId { node: NodeId, uri: Option<String>, server: u32 },
    QualifiedName(u16, Option<String>),
    Enum(i32),
}

#[derive(Clone, Debug, PartialEq)]
pub struct NodeId {
    pub ns: u16,
    pub id: Identifier,
}

#[derive(Clone, Debug, PartialEq)]
pub enum Identifier {
    Numeric(u32),
    String(Option<String>),
    Guid([u8; 16]),
    Opaque(Option<Vec<u8>>),
}

/// Decode the value a plan selects from a structure body, and render it as a sample value
/// with its `raw` bytes.
pub fn decode(
    plan: &Plan,
    types: &TypeSet,
    body: &[u8],
    namespaces: &[String],
) -> Result<(Value, Vec<u8>), String> {
    let mut r = Reader { buf: body, pos: 0, ctx: String::new() };
    for (n, step) in plan.steps.iter().enumerate() {
        let def = types.get(&step.key)?;
        let field = &def.fields[step.field];
        r.ctx = field.name.clone();
        match def.kind {
            StructureKind::Structure => {
                for f in &def.fields[..step.field] {
                    r.ctx = f.name.clone();
                    r.skip_field(types, f, 0)?;
                }
            }
            StructureKind::OptionalFields => {
                let mask = r.u32()?;
                let mut bit = 0;
                for f in &def.fields[..step.field] {
                    let present = !f.optional || mask & (1 << bit) != 0;
                    if f.optional {
                        bit += 1;
                    }
                    if present {
                        r.ctx = f.name.clone();
                        r.skip_field(types, f, 0)?;
                    }
                }
                if field.optional && mask & (1 << bit) == 0 {
                    return Err(format!("optional field \"{}\" is absent", field.name));
                }
            }
            StructureKind::Union => {
                let switch = r.u32()?;
                if switch as usize > def.fields.len() {
                    return Err(format!("invalid union switch {switch} in {}", def.name));
                }
                if switch as usize != step.field + 1 {
                    let active = match switch {
                        0 => "none".to_string(),
                        s => format!("\"{}\"", def.fields[s as usize - 1].name),
                    };
                    return Err(format!(
                        "union field \"{}\" is not set (active: {active})",
                        field.name
                    ));
                }
            }
        }
        r.ctx = field.name.clone();
        if let Some(index) = step.index {
            let len = r.len()?;
            if index as usize >= len {
                return Err(format!(
                    "index {index} out of range for \"{}\" (length {len})",
                    field.name
                ));
            }
            r.skip_elements(types, &field.ty, index as usize, 0)?;
        }
        if n + 1 == plan.steps.len() {
            let scalar = r.scalar(&plan.leaf)?;
            return render(&scalar, plan.datatype, namespaces);
        }
    }
    Err("field path is empty".into())
}

/// Decode a value of a built-in type on its own (an array element or a top-level variable's
/// binary encoding) and render it.
pub fn decode_builtin(
    b: Builtin,
    body: &[u8],
    dt: DataType,
    namespaces: &[String],
) -> Result<(Value, Vec<u8>), String> {
    let leaf = Leaf::Builtin(b);
    check_datatype(&leaf, dt, "value")?;
    let mut r = Reader { buf: body, pos: 0, ctx: "value".into() };
    let scalar = r.scalar(&leaf)?;
    render(&scalar, dt, namespaces)
}

/// Split a binary-encoded ExtensionObject into its encoding id and body (Part 6 §5.2.2.15).
pub fn split_extension_object(bytes: &[u8]) -> Result<(NodeId, Vec<u8>), String> {
    let mut r = Reader { buf: bytes, pos: 0, ctx: "value".into() };
    let enc = r.u8()?;
    if enc & 0xC0 != 0 {
        return Err(r.bad_encoding("NodeId", enc));
    }
    let id = r.node_id_body(enc)?;
    match r.u8()? {
        0 => Ok((id, Vec::new())),
        1 => Ok((id, r.bytes()?.unwrap_or_default().to_vec())),
        2 => Err("the value is an XML-encoded structure, which is not supported".into()),
        enc => Err(r.bad_encoding("ExtensionObject", enc)),
    }
}

/// Parse a binary-encoded NodeId.
pub fn read_node_id(bytes: &[u8]) -> Result<NodeId, String> {
    let mut r = Reader { buf: bytes, pos: 0, ctx: "NodeId".into() };
    let enc = r.u8()?;
    r.node_id_body(enc)
}

struct Reader<'a> {
    buf: &'a [u8],
    pos: usize,
    /// The field being read, for error messages.
    ctx: String,
}

impl<'a> Reader<'a> {
    fn take(&mut self, n: usize) -> Result<&'a [u8], String> {
        let end = self.pos.checked_add(n).filter(|end| *end <= self.buf.len());
        match end {
            Some(end) => {
                let out = &self.buf[self.pos..end];
                self.pos = end;
                Ok(out)
            }
            None => Err(format!("structure body truncated while reading \"{}\"", self.ctx)),
        }
    }

    fn array<const N: usize>(&mut self) -> Result<[u8; N], String> {
        let mut out = [0u8; N];
        out.copy_from_slice(self.take(N)?);
        Ok(out)
    }

    fn u8(&mut self) -> Result<u8, String> {
        Ok(self.take(1)?[0])
    }
    fn u16(&mut self) -> Result<u16, String> {
        Ok(u16::from_le_bytes(self.array()?))
    }
    fn u32(&mut self) -> Result<u32, String> {
        Ok(u32::from_le_bytes(self.array()?))
    }
    fn i32(&mut self) -> Result<i32, String> {
        Ok(i32::from_le_bytes(self.array()?))
    }
    fn i64(&mut self) -> Result<i64, String> {
        Ok(i64::from_le_bytes(self.array()?))
    }

    /// An Int32 length prefix: -1 is null (no elements), anything below that is invalid.
    fn len(&mut self) -> Result<usize, String> {
        match self.i32()? {
            -1 => Ok(0),
            n if n < 0 => Err(format!("invalid length {n} while reading \"{}\"", self.ctx)),
            n => Ok(n as usize),
        }
    }

    /// A length-prefixed byte run; `None` for the null encoding.
    fn bytes(&mut self) -> Result<Option<&'a [u8]>, String> {
        match self.i32()? {
            -1 => Ok(None),
            n if n < 0 => Err(format!("invalid length {n} while reading \"{}\"", self.ctx)),
            n => Ok(Some(self.take(n as usize)?)),
        }
    }

    fn string(&mut self) -> Result<Option<String>, String> {
        match self.bytes()? {
            None => Ok(None),
            Some(b) => String::from_utf8(b.to_vec())
                .map(Some)
                .map_err(|_| format!("invalid UTF-8 while reading \"{}\"", self.ctx)),
        }
    }

    fn deeper(&self, depth: usize) -> Result<usize, String> {
        if depth >= MAX_DEPTH {
            return Err(format!("nesting too deep while reading \"{}\"", self.ctx));
        }
        Ok(depth + 1)
    }

    fn skip_field(&mut self, types: &TypeSet, f: &Field, depth: usize) -> Result<(), String> {
        if f.array {
            let len = self.len()?;
            self.skip_elements(types, &f.ty, len, depth)
        } else {
            self.skip_value(types, &f.ty, depth)
        }
    }

    /// Skip `count` array elements. A structure without fields encodes to nothing, so once an
    /// element consumed no bytes every remaining one is the same: stop, rather than loop up to
    /// 2^31 times on a forged length.
    fn skip_elements(&mut self, types: &TypeSet, ty: &FieldType, count: usize, depth: usize) -> Result<(), String> {
        for _ in 0..count {
            let before = self.pos;
            self.skip_value(types, ty, depth)?;
            if self.pos == before {
                break;
            }
        }
        Ok(())
    }

    fn skip_value(&mut self, types: &TypeSet, ty: &FieldType, depth: usize) -> Result<(), String> {
        match ty {
            FieldType::Builtin(b) => self.skip_builtin(*b, depth),
            FieldType::Enum(_) => self.take(4).map(drop),
            FieldType::Struct(key) => {
                let depth = self.deeper(depth)?;
                let def = types.get(key)?;
                match def.kind {
                    StructureKind::Structure => {
                        for f in &def.fields {
                            self.skip_field(types, f, depth)?;
                        }
                    }
                    StructureKind::OptionalFields => {
                        let mask = self.u32()?;
                        let mut bit = 0;
                        for f in &def.fields {
                            let present = !f.optional || mask & (1 << bit) != 0;
                            if f.optional {
                                bit += 1;
                            }
                            if present {
                                self.skip_field(types, f, depth)?;
                            }
                        }
                    }
                    StructureKind::Union => match self.u32()? {
                        0 => {}
                        s if s as usize <= def.fields.len() => {
                            self.skip_field(types, &def.fields[s as usize - 1], depth)?
                        }
                        s => return Err(format!("invalid union switch {s} in {}", def.name)),
                    },
                }
                Ok(())
            }
        }
    }

    fn skip_builtin(&mut self, b: Builtin, depth: usize) -> Result<(), String> {
        match b {
            Builtin::Boolean | Builtin::SByte | Builtin::Byte => self.take(1).map(drop),
            Builtin::Int16 | Builtin::UInt16 => self.take(2).map(drop),
            Builtin::Int32 | Builtin::UInt32 | Builtin::Float | Builtin::StatusCode => {
                self.take(4).map(drop)
            }
            Builtin::Int64 | Builtin::UInt64 | Builtin::Double | Builtin::DateTime => {
                self.take(8).map(drop)
            }
            Builtin::Guid => self.take(16).map(drop),
            Builtin::String | Builtin::ByteString | Builtin::XmlElement => self.bytes().map(drop),
            Builtin::NodeId => {
                let enc = self.u8()?;
                if enc & 0xC0 != 0 {
                    return Err(self.bad_encoding("NodeId", enc));
                }
                self.node_id_body(enc).map(drop)
            }
            Builtin::ExpandedNodeId => self.expanded_node_id().map(drop),
            Builtin::QualifiedName => {
                self.take(2)?;
                self.bytes().map(drop)
            }
            Builtin::LocalizedText => {
                let mask = self.u8()?;
                if mask & 0x01 != 0 {
                    self.bytes()?;
                }
                if mask & 0x02 != 0 {
                    self.bytes()?;
                }
                Ok(())
            }
            Builtin::ExtensionObject => {
                self.skip_builtin(Builtin::NodeId, depth)?;
                match self.u8()? {
                    0 => Ok(()),
                    1 | 2 => self.bytes().map(drop),
                    enc => Err(self.bad_encoding("ExtensionObject", enc)),
                }
            }
            Builtin::DataValue => {
                let depth = self.deeper(depth)?;
                let mask = self.u8()?;
                if mask & 0x01 != 0 {
                    self.skip_builtin(Builtin::Variant, depth)?;
                }
                if mask & 0x02 != 0 {
                    self.take(4)?;
                }
                if mask & 0x04 != 0 {
                    self.take(8)?;
                }
                if mask & 0x10 != 0 {
                    self.take(2)?;
                }
                if mask & 0x08 != 0 {
                    self.take(8)?;
                }
                if mask & 0x20 != 0 {
                    self.take(2)?;
                }
                Ok(())
            }
            Builtin::Variant => {
                let depth = self.deeper(depth)?;
                let mask = self.u8()?;
                let id = (mask & 0x3F) as u32;
                if id == 0 {
                    return Ok(());
                }
                let inner = Builtin::from_id(id).ok_or_else(|| self.bad_encoding("Variant", mask))?;
                if mask & 0x80 != 0 {
                    for _ in 0..self.len()? {
                        self.skip_builtin(inner, depth)?;
                    }
                    if mask & 0x40 != 0 {
                        for _ in 0..self.len()? {
                            self.take(4)?;
                        }
                    }
                    Ok(())
                } else {
                    self.skip_builtin(inner, depth)
                }
            }
            Builtin::DiagnosticInfo => {
                let depth = self.deeper(depth)?;
                let mask = self.u8()?;
                for bit in [0x01, 0x02, 0x04, 0x08] {
                    if mask & bit != 0 {
                        self.take(4)?;
                    }
                }
                if mask & 0x10 != 0 {
                    self.bytes()?;
                }
                if mask & 0x20 != 0 {
                    self.take(4)?;
                }
                if mask & 0x40 != 0 {
                    self.skip_builtin(Builtin::DiagnosticInfo, depth)?;
                }
                Ok(())
            }
        }
    }

    fn bad_encoding(&self, what: &str, byte: u8) -> String {
        format!("invalid {what} encoding 0x{byte:02x} while reading \"{}\"", self.ctx)
    }

    /// The NodeId after its encoding byte (only the low 6 bits select the form).
    fn node_id_body(&mut self, enc: u8) -> Result<NodeId, String> {
        Ok(match enc & 0x3F {
            0 => NodeId { ns: 0, id: Identifier::Numeric(self.u8()? as u32) },
            1 => {
                let ns = self.u8()? as u16;
                NodeId { ns, id: Identifier::Numeric(self.u16()? as u32) }
            }
            2 => {
                let ns = self.u16()?;
                NodeId { ns, id: Identifier::Numeric(self.u32()?) }
            }
            3 => {
                let ns = self.u16()?;
                NodeId { ns, id: Identifier::String(self.string()?) }
            }
            4 => {
                let ns = self.u16()?;
                NodeId { ns, id: Identifier::Guid(self.array()?) }
            }
            5 => {
                let ns = self.u16()?;
                NodeId { ns, id: Identifier::Opaque(self.bytes()?.map(<[u8]>::to_vec)) }
            }
            _ => return Err(self.bad_encoding("NodeId", enc)),
        })
    }

    fn expanded_node_id(&mut self) -> Result<Scalar, String> {
        let enc = self.u8()?;
        let node = self.node_id_body(enc)?;
        let uri = if enc & 0x80 != 0 { self.string()? } else { None };
        let server = if enc & 0x40 != 0 { self.u32()? } else { 0 };
        Ok(Scalar::ExpandedNodeId { node, uri, server })
    }

    fn scalar(&mut self, leaf: &Leaf) -> Result<Scalar, String> {
        let b = match leaf {
            Leaf::Enum(_) => return Ok(Scalar::Enum(self.i32()?)),
            Leaf::Builtin(b) => *b,
        };
        Ok(match b {
            Builtin::Boolean => Scalar::Bool(self.u8()? != 0),
            Builtin::SByte => Scalar::I8(self.u8()? as i8),
            Builtin::Byte => Scalar::U8(self.u8()?),
            Builtin::Int16 => Scalar::I16(i16::from_le_bytes(self.array()?)),
            Builtin::UInt16 => Scalar::U16(self.u16()?),
            Builtin::Int32 => Scalar::I32(self.i32()?),
            Builtin::UInt32 => Scalar::U32(self.u32()?),
            Builtin::Int64 => Scalar::I64(self.i64()?),
            Builtin::UInt64 => Scalar::U64(u64::from_le_bytes(self.array()?)),
            Builtin::Float => Scalar::F32(f32::from_le_bytes(self.array()?)),
            Builtin::Double => Scalar::F64(f64::from_le_bytes(self.array()?)),
            Builtin::String => Scalar::Text(self.string()?),
            Builtin::DateTime => Scalar::DateTime(self.i64()?),
            Builtin::Guid => Scalar::Guid(self.array()?),
            Builtin::ByteString => Scalar::ByteString(self.bytes()?.map(<[u8]>::to_vec)),
            Builtin::StatusCode => Scalar::StatusCode(self.u32()?),
            Builtin::NodeId => {
                let enc = self.u8()?;
                if enc & 0xC0 != 0 {
                    return Err(self.bad_encoding("NodeId", enc));
                }
                Scalar::NodeId(self.node_id_body(enc)?)
            }
            Builtin::ExpandedNodeId => self.expanded_node_id()?,
            Builtin::QualifiedName => {
                let ns = self.u16()?;
                Scalar::QualifiedName(ns, self.string()?)
            }
            Builtin::LocalizedText => {
                let mask = self.u8()?;
                if mask & 0x01 != 0 {
                    self.string()?;
                }
                let text = if mask & 0x02 != 0 { self.string()? } else { None };
                Scalar::Text(text)
            }
            other => return Err(format!("{} cannot be read as a point", other.name())),
        })
    }
}

/// Render a decoded value as the point's `datatype`, with its `raw` bytes: big-endian for the
/// numeric forms, the UTF-8 of the text forms, the bytes themselves for a ByteString.
pub fn render(scalar: &Scalar, dt: DataType, namespaces: &[String]) -> Result<(Value, Vec<u8>), String> {
    let text = |s: String| {
        let raw = s.clone().into_bytes();
        Ok((Value::Text(s), raw))
    };
    match scalar {
        Scalar::Bool(b) => Ok((Value::Bool(*b), vec![*b as u8])),
        Scalar::I8(v) => Ok((Value::Number(*v as f64), v.to_be_bytes().to_vec())),
        Scalar::U8(v) => Ok((Value::Number(*v as f64), vec![*v])),
        Scalar::I16(v) => Ok((Value::Number(*v as f64), v.to_be_bytes().to_vec())),
        Scalar::U16(v) => Ok((Value::Number(*v as f64), v.to_be_bytes().to_vec())),
        Scalar::I32(v) | Scalar::Enum(v) => Ok((Value::Number(*v as f64), v.to_be_bytes().to_vec())),
        Scalar::U32(v) => Ok((Value::Number(*v as f64), v.to_be_bytes().to_vec())),
        Scalar::I64(v) => Ok((crate::int64_value(*v), v.to_be_bytes().to_vec())),
        Scalar::U64(v) => Ok((crate::uint64_value(*v), v.to_be_bytes().to_vec())),
        Scalar::F32(v) => Ok((Value::Number(*v as f64), v.to_be_bytes().to_vec())),
        Scalar::F64(v) => Ok((Value::Number(*v), v.to_be_bytes().to_vec())),
        Scalar::Text(s) => text(s.clone().unwrap_or_default()),
        Scalar::DateTime(ticks) => {
            let ticks = (*ticks).clamp(0, MAX_TICKS);
            if dt == DataType::Int64 {
                let ms = (ticks - UNIX_EPOCH_TICKS).div_euclid(10_000);
                Ok((crate::int64_value(ms), ticks.to_be_bytes().to_vec()))
            } else {
                text(datetime_text(ticks))
            }
        }
        Scalar::StatusCode(code) => {
            if dt == DataType::Uint32 {
                Ok((Value::Number(*code as f64), code.to_be_bytes().to_vec()))
            } else {
                text(status_code_text(*code))
            }
        }
        Scalar::Guid(g) => text(guid_text(g)),
        Scalar::ByteString(b) => {
            let b = b.clone().unwrap_or_default();
            Ok((Value::Text(hex(&b)), b))
        }
        Scalar::NodeId(n) => text(node_id_text(n, namespaces)),
        Scalar::ExpandedNodeId { node, uri, server } => {
            let mut s = String::new();
            if *server != 0 {
                s.push_str(&format!("svr={server};"));
            }
            match uri {
                Some(uri) => {
                    s.push_str(&format!("nsu={uri};"));
                    s.push_str(&identifier_text(&node.id));
                }
                None => s.push_str(&node_id_text(node, namespaces)),
            }
            text(s)
        }
        Scalar::QualifiedName(ns, name) => {
            let name = name.clone().unwrap_or_default();
            text(if *ns == 0 { name } else { format!("{ns}:{name}") })
        }
    }
}

/// RFC 3339 UTC with up to 7 fraction digits, trailing zeros trimmed. `ticks` must be clamped.
pub fn datetime_text(ticks: i64) -> String {
    let secs = ticks.div_euclid(10_000_000) - UNIX_EPOCH_TICKS / 10_000_000;
    let frac = ticks.rem_euclid(10_000_000);
    let (y, mo, d) = civil_from_days(secs.div_euclid(86_400));
    let sod = secs.rem_euclid(86_400);
    let mut s = format!(
        "{y:04}-{mo:02}-{d:02}T{:02}:{:02}:{:02}",
        sod / 3600,
        sod / 60 % 60,
        sod % 60
    );
    if frac != 0 {
        let digits = format!("{frac:07}");
        s.push('.');
        s.push_str(digits.trim_end_matches('0'));
    }
    s.push('Z');
    s
}

/// Days since 1970-01-01 to a proleptic Gregorian date (H. Hinnant's `civil_from_days`).
fn civil_from_days(z: i64) -> (i64, i64, i64) {
    let z = z + 719_468;
    let era = z.div_euclid(146_097);
    let doe = z.rem_euclid(146_097);
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = doy - (153 * mp + 2) / 5 + 1;
    let m = if mp < 10 { mp + 3 } else { mp - 9 };
    let y = yoe + era * 400 + if m <= 2 { 1 } else { 0 };
    (y, m, d)
}

/// The symbolic name of a status code, or `0x` and 8 hex digits when it has none. A code with
/// any of the low 16 (info) bits set has no name.
pub fn status_code_text(code: u32) -> String {
    use opcua::types::status_code::SubStatusCode;
    if code & 0xFFFF == 0 {
        if let Some(sub) = SubStatusCode::from_value(code) {
            return sub.name().to_string();
        }
    }
    format!("0x{code:08X}")
}

/// `xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx` from the 16 bytes as encoded.
pub fn guid_text(g: &[u8; 16]) -> String {
    let d1 = u32::from_le_bytes([g[0], g[1], g[2], g[3]]);
    let d2 = u16::from_le_bytes([g[4], g[5]]);
    let d3 = u16::from_le_bytes([g[6], g[7]]);
    format!("{d1:08x}-{d2:04x}-{d3:04x}-{}-{}", hex(&g[8..10]), hex(&g[10..16]))
}

fn identifier_text(id: &Identifier) -> String {
    match id {
        Identifier::Numeric(n) => format!("i={n}"),
        Identifier::String(s) => format!("s={}", s.as_deref().unwrap_or("")),
        Identifier::Guid(g) => format!("g={}", guid_text(g)),
        Identifier::Opaque(b) => format!("b={}", base64(b.as_deref().unwrap_or(&[]))),
    }
}

/// Part 6 text form, with `nsu=<uri>;` for a namespace other than 0 that the server lists.
pub fn node_id_text(n: &NodeId, namespaces: &[String]) -> String {
    let prefix = match n.ns {
        0 => String::new(),
        ns => match namespaces.get(ns as usize) {
            Some(uri) => format!("nsu={uri};"),
            None => format!("ns={ns};"),
        },
    };
    prefix + &identifier_text(&n.id)
}

fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

fn base64(bytes: &[u8]) -> String {
    const ALPHABET: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut out = String::new();
    for chunk in bytes.chunks(3) {
        let n = chunk.iter().enumerate().fold(0u32, |n, (i, b)| n | (*b as u32) << (16 - 8 * i));
        for i in 0..4 {
            if i <= chunk.len() {
                out.push(ALPHABET[(n >> (18 - 6 * i) & 0x3F) as usize] as char);
            } else {
                out.push('=');
            }
        }
    }
    out
}

/// Build a [`TypeSet`] from the `types` of the shared vectors
/// (`doc/contract/test-vectors/opcua-struct/README.md`), keyed by type name. Used by the vector
/// runner, the property test and the fuzz target.
pub fn type_set_from_json(v: &serde_json::Value) -> Result<TypeSet, String> {
    let defs = v["types"].as_object().ok_or("no types")?;
    let mut set = TypeSet::default();
    for (name, t) in defs {
        if t.get("enum").is_some() {
            continue;
        }
        let kind = match t["structure_type"].as_str() {
            Some("Structure") => StructureKind::Structure,
            Some("StructureWithOptionalFields") => StructureKind::OptionalFields,
            Some("Union") => StructureKind::Union,
            other => return Err(format!("{name}: structure_type {other:?}")),
        };
        let mut fields = Vec::new();
        for f in t["fields"].as_array().ok_or("no fields")? {
            let ty = f["type"].as_str().ok_or("no field type")?;
            let ty = match Builtin::from_name(ty) {
                Some(b) => FieldType::Builtin(b),
                None if defs.get(ty).and_then(|d| d.get("enum")).is_some() => FieldType::Enum(ty.into()),
                None => FieldType::Struct(ty.into()),
            };
            fields.push(Field {
                name: f["name"].as_str().ok_or("no field name")?.into(),
                ty,
                array: f["array"].as_bool().unwrap_or(false),
                optional: f["optional"].as_bool().unwrap_or(false),
            });
        }
        set.structs.insert(name.clone(), StructDef { name: name.clone(), kind, fields });
    }
    Ok(set)
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::Value as Json;

    const VECTORS: &str = include_str!("../../../../../doc/contract/test-vectors/opcua-struct/vectors.json");

    fn datatype(s: &str) -> DataType {
        serde_json::from_value(Json::String(s.into())).unwrap()
    }

    fn run(v: &Json, types: &TypeSet, ns: &[String]) -> Result<(Value, Vec<u8>), String> {
        let body = hex_decode(v["body"].as_str().unwrap());
        let dt = datatype(v["datatype"].as_str().unwrap());
        let root = v["root"].as_str().unwrap();
        match Builtin::from_name(root) {
            Some(b) => decode_builtin(b, &body, dt, ns),
            None => {
                let path = parse_path(v["path"].as_str().unwrap())?;
                let plan = compile(types, root, &path, dt)?;
                decode(&plan, types, &body, ns)
            }
        }
    }

    fn hex_decode(s: &str) -> Vec<u8> {
        (0..s.len()).step_by(2).map(|i| u8::from_str_radix(&s[i..i + 2], 16).unwrap()).collect()
    }

    #[test]
    fn shared_read_vectors() {
        let v: Json = serde_json::from_str(VECTORS).unwrap();
        let types = type_set_from_json(&v).unwrap();
        let ns: Vec<String> = v["namespaces"]
            .as_array()
            .unwrap()
            .iter()
            .map(|s| s.as_str().unwrap().to_string())
            .collect();
        let cases = v["read"].as_array().unwrap();
        assert!(cases.len() >= 70, "only {} read vectors", cases.len());
        let mut failures = Vec::new();
        for case in cases {
            let name = case["name"].as_str().unwrap();
            let got = run(case, &types, &ns);
            let ok = match (&got, case.get("error")) {
                (Err(e), Some(want)) => e == want.as_str().unwrap(),
                (Ok((value, raw)), None) => {
                    let out = case["out"].as_object().unwrap();
                    let (k, want) = out.iter().next().unwrap();
                    let value_ok = match (k.as_str(), value) {
                        ("num", Value::Number(n)) => *n == want.as_f64().unwrap(),
                        ("str" | "hex", Value::Text(s)) => s == want.as_str().unwrap(),
                        ("bool", Value::Bool(b)) => *b == want.as_bool().unwrap(),
                        _ => false,
                    };
                    value_ok && hex(raw) == case["raw"].as_str().unwrap()
                }
                _ => false,
            };
            if !ok {
                failures.push(format!("{name}: got {got:?}"));
            }
        }
        assert!(failures.is_empty(), "{} vectors failed:\n{}", failures.len(), failures.join("\n"));
    }

    #[test]
    fn shared_invalid_paths() {
        let v: Json = serde_json::from_str(VECTORS).unwrap();
        for case in v["invalid_paths"].as_array().unwrap() {
            let got = parse_path(case["path"].as_str().unwrap());
            assert_eq!(got.err().as_deref(), case["error"].as_str(), "{case}");
        }
    }

    #[test]
    fn paths_parse() {
        assert_eq!(
            parse_path("Items[12].Value").unwrap(),
            vec![
                Segment { name: "Items".into(), index: Some(12) },
                Segment { name: "Value".into(), index: None },
            ]
        );
    }

    #[test]
    fn base64_matches_rfc4648() {
        assert_eq!(base64(b""), "");
        assert_eq!(base64(b"f"), "Zg==");
        assert_eq!(base64(b"fo"), "Zm8=");
        assert_eq!(base64(b"foo"), "Zm9v");
        assert_eq!(base64(b"foobar"), "Zm9vYmFy");
    }

    /// Every path the vectors compile, over arbitrary and mutated bodies: decoding returns a
    /// value or an error, and never panics or reads past the body.
    mod never_panics {
        use super::*;
        use proptest::prelude::*;

        /// The vectors' types and namespaces, and every plan they compile with its body.
        struct Cases {
            types: TypeSet,
            ns: Vec<String>,
            plans: Vec<(Plan, Vec<u8>)>,
        }

        fn cases() -> Cases {
            let v: Json = serde_json::from_str(VECTORS).unwrap();
            let types = type_set_from_json(&v).unwrap();
            let ns = v["namespaces"].as_array().unwrap().iter().map(|s| s.as_str().unwrap().into()).collect();
            let plans = v["read"]
                .as_array()
                .unwrap()
                .iter()
                .filter_map(|c| {
                    let path = parse_path(c["path"].as_str()?).ok()?;
                    let plan = compile(&types, c["root"].as_str()?, &path, datatype(c["datatype"].as_str()?)).ok()?;
                    Some((plan, hex_decode(c["body"].as_str()?)))
                })
                .collect();
            Cases { types, ns, plans }
        }

        proptest! {
            #![proptest_config(ProptestConfig::with_cases(2000))]

            #[test]
            fn arbitrary_bodies(pick in any::<prop::sample::Index>(), body in prop::collection::vec(any::<u8>(), 0..200)) {
                let c = cases();
                let (plan, _) = &c.plans[pick.index(c.plans.len())];
                let _ = decode(plan, &c.types, &body, &c.ns);
            }

            #[test]
            fn mutated_bodies(pick in any::<prop::sample::Index>(), at in any::<prop::sample::Index>(), byte in any::<u8>(), cut in any::<prop::sample::Index>()) {
                let c = cases();
                let (plan, body) = &c.plans[pick.index(c.plans.len())];
                let mut body = body.clone();
                if !body.is_empty() {
                    let i = at.index(body.len());
                    body[i] = byte;
                    body.truncate(cut.index(body.len() + 1));
                }
                let _ = decode(plan, &c.types, &body, &c.ns);
            }

            #[test]
            fn arbitrary_builtins(id in 1u32..=25, body in prop::collection::vec(any::<u8>(), 0..64)) {
                let b = Builtin::from_id(id).unwrap();
                for dt in accepted(&Leaf::Builtin(b)) {
                    let _ = decode_builtin(b, &body, *dt, &[]);
                }
                let _ = split_extension_object(&body);
            }
        }
    }

    #[test]
    fn empty_structure_arrays_do_not_spin() {
        let mut types = TypeSet::default();
        types.structs.insert("Empty".into(), StructDef { name: "Empty".into(), kind: StructureKind::Structure, fields: vec![] });
        types.structs.insert(
            "Holder".into(),
            StructDef {
                name: "Holder".into(),
                kind: StructureKind::Structure,
                fields: vec![
                    Field { name: "Many".into(), ty: FieldType::Struct("Empty".into()), array: true, optional: false },
                    Field { name: "Tail".into(), ty: FieldType::Builtin(Builtin::Byte), array: false, optional: false },
                ],
            },
        );
        let plan = compile(&types, "Holder", &parse_path("Tail").unwrap(), DataType::Uint8).unwrap();
        let mut body = i32::MAX.to_le_bytes().to_vec();
        body.push(7);
        let started = std::time::Instant::now();
        assert_eq!(decode(&plan, &types, &body, &[]).unwrap().0, Value::Number(7.0));
        assert!(started.elapsed() < std::time::Duration::from_secs(1));
    }

    #[test]
    fn deep_nesting_is_refused() {
        // A Variant holding a Variant array holding a Variant array ... deeper than MAX_DEPTH.
        let mut body = Vec::new();
        for _ in 0..(MAX_DEPTH + 2) {
            body.push(0x80 | Builtin::Variant as u8);
            body.extend_from_slice(&1i32.to_le_bytes());
        }
        body.push(0);
        let types = TypeSet::default();
        let mut r = Reader { buf: &body, pos: 0, ctx: "Var".into() };
        let err = r.skip_value(&types, &FieldType::Builtin(Builtin::Variant), 0).unwrap_err();
        assert_eq!(err, "nesting too deep while reading \"Var\"");
    }
}
