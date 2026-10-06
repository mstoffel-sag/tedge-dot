//! What a session learns from the server before structured values can be decoded
//! (openspec change `opcua-custom-datatypes`, design D3, D4 and D6): the namespace array, each
//! structure point's DataType, and the `DataTypeDefinition`s its path crosses or skips. It runs
//! once per session and is thrown away with it, so a server update is picked up on reconnect.
//! Only the types a configured point needs are read; the server's type tree is never browsed.

use std::collections::{HashMap, HashSet};
use std::future::Future;
use std::pin::Pin;

use opcua::client::Session;
use opcua::types::{
    AttributeId, BinaryDecodable, BinaryEncodable, BrowseDescription, BrowseDirection, DataValue, EnumDefinition, Identifier,
    NodeId, ReadValueId, ReferenceTypeId, StructureDefinition, StructureType,
    TimestampsToReturn, Variant,
};
use tedge_dot_sdk::DataType;

use crate::structure::{self, Builtin, Field, FieldType, Plan, Segment, StructDef, StructureKind, TypeSet};

/// Prefix of every reason a definition could not be resolved (design D6).
pub const UNAVAILABLE: &str = "data type definition unavailable";
/// Supertype hops followed to find the built-in type behind a simple subtype.
const MAX_SUPERTYPES: usize = 8;

/// Everything decoding needs for one session.
#[derive(Default)]
pub struct SessionTypes {
    /// The server's namespace array, for the `nsu=` forms.
    pub namespaces: Vec<String>,
    pub types: TypeSet,
    /// Per field point: its plan and the structure's binary encoding id, or the bad-sample
    /// reason.
    pub plans: HashMap<String, Result<FieldPlan, String>>,
    /// Per node read by a raw point: its DataType, for the `addr.data_type` echo.
    pub data_types: HashMap<NodeId, NodeId>,
}

pub struct FieldPlan {
    pub plan: Plan,
    /// The structure's default binary encoding, when the definition names one.
    pub encoding: Option<NodeId>,
}

/// A point that needs resolution.
pub struct Wanted {
    pub point: String,
    pub node: NodeId,
    /// `None` for a raw point, which only needs its DataType.
    pub field: Option<Vec<Segment>>,
    /// The array element the point reads, if any.
    pub index: Option<u32>,
    pub datatype: Option<DataType>,
}

pub async fn resolve(session: &Session, wanted: &[Wanted]) -> SessionTypes {
    let mut r = Resolver {
        session,
        out: SessionTypes::default(),
        encodings: HashMap::new(),
        failed: HashMap::new(),
        loading: HashSet::new(),
    };
    r.out.namespaces = r.namespaces().await;
    if wanted.is_empty() {
        return r.out;
    }

    let nodes: Vec<NodeId> = {
        let mut seen = HashSet::new();
        wanted.iter().filter(|w| seen.insert(w.node.clone())).map(|w| w.node.clone()).collect()
    };
    let data_types = r.data_types(&nodes).await;
    for w in wanted {
        let data_type = data_types.get(&w.node).cloned().unwrap_or_else(|| Err("not read".into()));
        let Some(path) = &w.field else {
            if let Ok(dt) = data_type {
                r.out.data_types.insert(w.node.clone(), dt);
            }
            continue;
        };
        // A variable may be declared with the abstract Structure (or BaseDataType): the concrete
        // type is then the one its value is encoded as.
        let data_type = match data_type {
            Ok(dt) if matches!(builtin_of(&dt), Some(Builtin::ExtensionObject | Builtin::Variant)) => {
                r.concrete_type(&w.node, w.index).await
            }
            other => other,
        };
        let plan = match data_type {
            Err(e) => Err(format!("{UNAVAILABLE}: {e}")),
            Ok(dt) => match r.structure(&dt).await {
                Err(e) => Err(format!("{UNAVAILABLE}: {e}")),
                Ok(root) => {
                    let datatype = w.datatype.unwrap_or(DataType::Float64);
                    structure::compile(&r.out.types, &root, path, datatype).map(|plan| FieldPlan {
                        plan,
                        encoding: r.encodings.get(&root).cloned(),
                    })
                }
            },
        };
        r.out.plans.insert(w.point.clone(), plan);
    }
    r.out
}

struct Resolver<'a> {
    session: &'a Session,
    out: SessionTypes,
    /// Default binary encoding per structure key.
    encodings: HashMap<String, NodeId>,
    /// Structures whose definition could not be resolved, with the reason.
    failed: HashMap<String, String>,
    /// Structures being resolved, so a recursive definition does not loop.
    loading: HashSet<String>,
}

type BoxFuture<'f, T> = Pin<Box<dyn Future<Output = T> + Send + 'f>>;

impl<'a> Resolver<'a> {
    async fn read(&self, reads: Vec<ReadValueId>) -> Result<Vec<DataValue>, String> {
        self.session
            .read(&reads, TimestampsToReturn::Neither, 0.0)
            .await
            .map_err(|s| format!("read failed: {s}"))
    }

    async fn namespaces(&self) -> Vec<String> {
        let read = vec![value_of(NodeId::new(0, 2255u32), AttributeId::Value)];
        let Ok(values) = self.read(read).await else { return Vec::new() };
        match values.into_iter().next().and_then(|dv| dv.value) {
            Some(Variant::Array(array)) => array
                .values
                .into_iter()
                .map(|v| match v {
                    Variant::String(s) => s.as_ref().to_string(),
                    _ => String::new(),
                })
                .collect(),
            _ => Vec::new(),
        }
    }

    /// The DataType attribute of every node, in one request.
    async fn data_types(&self, nodes: &[NodeId]) -> HashMap<NodeId, Result<NodeId, String>> {
        let reads = nodes.iter().map(|n| value_of(n.clone(), AttributeId::DataType)).collect();
        let values = match self.read(reads).await {
            Ok(v) => v,
            Err(e) => return nodes.iter().map(|n| (n.clone(), Err(e.clone()))).collect(),
        };
        nodes
            .iter()
            .zip(values)
            .map(|(n, dv)| {
                let dt = match (dv.status, dv.value) {
                    (Some(s), _) if !s.is_good() => Err(format!("reading the DataType of {n}: {s}")),
                    (_, Some(Variant::NodeId(id))) => Ok(*id),
                    _ => Err(format!("{n} has no DataType")),
                };
                (n.clone(), dt)
            })
            .collect()
    }

    /// The DataType of the structure a variable currently holds: its value's encoding id, and
    /// the DataType that encoding belongs to (the inverse of `HasEncoding`).
    async fn concrete_type(&self, node: &NodeId, index: Option<u32>) -> Result<NodeId, String> {
        let mut read = value_of(node.clone(), AttributeId::Value);
        if let Some(i) = index {
            read.index_range = opcua::types::NumericRange::Index(i);
        }
        let value = self.read(vec![read]).await?.into_iter().next().and_then(|dv| dv.value);
        let value = match value {
            Some(Variant::Array(array)) => array.values.into_iter().next(),
            other => other,
        };
        let Some(Variant::ExtensionObject(eo)) = value else {
            return Err(format!("{node} is declared with an abstract DataType and does not hold a structure"));
        };
        let ctx = crate::encoding_context();
        let bytes = eo.encode_to_vec(&ctx);
        let encoding = <NodeId as BinaryDecodable>::decode(&mut std::io::Cursor::new(&bytes), &ctx)
            .map_err(|e| format!("decoding the encoding id of {node}: {e}"))?;
        let browse = BrowseDescription {
            node_id: encoding.clone(),
            browse_direction: BrowseDirection::Inverse,
            reference_type_id: ReferenceTypeId::HasEncoding.into(),
            include_subtypes: false,
            node_class_mask: 0,
            result_mask: 0x3F,
        };
        self.session
            .browse(&[browse], 1, None)
            .await
            .map_err(|s| format!("browsing the DataType of encoding {encoding}: {s}"))?
            .into_iter()
            .next()
            .and_then(|r| r.references)
            .and_then(|refs| refs.into_iter().next())
            .map(|r| r.node_id.node_id)
            .ok_or_else(|| {
                format!("{node} is declared with an abstract DataType, and the server names no DataType for its encoding {encoding}")
            })
    }

    /// Resolve a structure type and every type it contains, returning its key in the type set.
    fn structure<'s>(&'s mut self, dt: &'s NodeId) -> BoxFuture<'s, Result<String, String>>
    where
        'a: 's,
    {
        Box::pin(async move {
            let key = dt.to_string();
            if self.out.types.structs.contains_key(&key) || self.loading.contains(&key) {
                return Ok(key);
            }
            if let Some(e) = self.failed.get(&key) {
                return Err(e.clone());
            }
            let result = self.load_structure(dt, &key).await;
            match &result {
                Ok(def) => {
                    self.out.types.structs.insert(key.clone(), def.clone());
                }
                Err(e) => {
                    self.failed.insert(key.clone(), e.clone());
                }
            }
            result.map(|_| key)
        })
    }

    async fn load_structure(&mut self, dt: &NodeId, key: &str) -> Result<StructDef, String> {
        let (name, definition) = self.definition(dt).await?;
        let def = match definition {
            Some(Definition::Structure(def)) => def,
            Some(Definition::Enum) => return Err(format!("{name} is an Enumeration, not a structure")),
            None => {
                return Err(match builtin_of(dt) {
                    Some(b) => format!("the value's DataType is {}, not a structure", b.name()),
                    None => format!("{name} ({dt}) has no DataTypeDefinition"),
                })
            }
        };
        let kind = match def.structure_type {
            StructureType::Structure => StructureKind::Structure,
            StructureType::StructureWithOptionalFields => StructureKind::OptionalFields,
            StructureType::Union => StructureKind::Union,
            other => return Err(format!("{name} is a {other:?}, which is not supported")),
        };
        if !def.default_encoding_id.is_null() {
            self.encodings.insert(key.to_string(), def.default_encoding_id.clone());
        }
        self.loading.insert(key.to_string());
        let mut fields = Vec::new();
        for f in def.fields.unwrap_or_default() {
            let fname = f.name.as_ref().to_string();
            let array = match f.value_rank {
                -1 => false,
                1 => true,
                rank => {
                    self.loading.remove(key);
                    return Err(format!(
                        "field \"{fname}\" of {name} has ValueRank {rank}, which is not supported"
                    ));
                }
            };
            let ty = match self.field_type(&f.data_type).await {
                Ok(ty) => ty,
                Err(e) => {
                    self.loading.remove(key);
                    return Err(format!("field \"{fname}\" of {name}: {e}"));
                }
            };
            fields.push(Field { name: fname, ty, array, optional: f.is_optional });
        }
        self.loading.remove(key);
        Ok(StructDef { name, kind, fields })
    }

    /// The type of a structure field: a built-in type, an Enumeration or a structure.
    async fn field_type(&mut self, dt: &NodeId) -> Result<FieldType, String> {
        if let Some(b) = builtin_of(dt) {
            return Ok(FieldType::Builtin(b));
        }
        let (name, definition) = self.definition(dt).await?;
        match definition {
            Some(Definition::Structure(_)) => Ok(FieldType::Struct(self.structure(dt).await?)),
            Some(Definition::Enum) => Ok(FieldType::Enum(name)),
            // A simple subtype (an alias of a built-in type): follow its supertypes.
            None => {
                let mut current = dt.clone();
                for _ in 0..MAX_SUPERTYPES {
                    current = self.supertype(&current).await?;
                    if current == NodeId::new(0, 29u32) {
                        return Ok(FieldType::Enum(name));
                    }
                    if let Some(b) = builtin_of(&current) {
                        return Ok(FieldType::Builtin(b));
                    }
                }
                Err(format!("{name} ({dt}) has no definition and no built-in supertype"))
            }
        }
    }

    /// A DataType node's browse name and definition, if it has one.
    async fn definition(&self, dt: &NodeId) -> Result<(String, Option<Definition>), String> {
        let reads = vec![
            value_of(dt.clone(), AttributeId::BrowseName),
            value_of(dt.clone(), AttributeId::DataTypeDefinition),
        ];
        let values = self.read(reads).await?;
        let name = match values.first().and_then(|dv| dv.value.as_ref()) {
            Some(Variant::QualifiedName(q)) => q.name.as_ref().to_string(),
            _ => dt.to_string(),
        };
        let definition = match values.get(1).and_then(|dv| dv.value.as_ref()) {
            Some(Variant::ExtensionObject(eo)) => {
                if let Some(def) = eo.inner_as::<StructureDefinition>() {
                    Some(Definition::Structure(def.clone()))
                } else if eo.inner_as::<EnumDefinition>().is_some() {
                    Some(Definition::Enum)
                } else {
                    None
                }
            }
            _ => None,
        };
        Ok((name, definition))
    }

    async fn supertype(&self, dt: &NodeId) -> Result<NodeId, String> {
        let browse = BrowseDescription {
            node_id: dt.clone(),
            browse_direction: BrowseDirection::Inverse,
            reference_type_id: ReferenceTypeId::HasSubtype.into(),
            include_subtypes: false,
            node_class_mask: 0,
            result_mask: 0x3F,
        };
        let results = self
            .session
            .browse(&[browse], 1, None)
            .await
            .map_err(|s| format!("browsing the supertype of {dt}: {s}"))?;
        results
            .into_iter()
            .next()
            .and_then(|r| r.references)
            .and_then(|refs| refs.into_iter().next())
            .map(|r| r.node_id.node_id)
            .ok_or_else(|| format!("{dt} has no supertype"))
    }
}

enum Definition {
    Structure(StructureDefinition),
    Enum,
}

fn value_of(node_id: NodeId, attribute: AttributeId) -> ReadValueId {
    ReadValueId {
        node_id,
        attribute_id: attribute as u32,
        index_range: Default::default(),
        data_encoding: Default::default(),
    }
}

/// The built-in type a namespace-0 DataType is encoded as, for the built-in types themselves
/// and their common simple subtypes (OPC UA Part 6 §5.1.2, Part 3 §8).
pub fn builtin_of(dt: &NodeId) -> Option<Builtin> {
    if dt.namespace != 0 {
        return None;
    }
    let Identifier::Numeric(id) = dt.identifier else { return None };
    let b = match id {
        1..=21 | 23 | 25 => return Builtin::from_id(id),
        22 => Builtin::ExtensionObject, // Structure: abstract, encoded as an ExtensionObject
        24 | 26..=28 => Builtin::Variant, // BaseDataType, Number, Integer, UInteger
        288 | 289 | 17588 | 20998 => Builtin::UInt32, // IntegerId, Counter, Index, VersionTime
        290 => Builtin::Double,                        // Duration
        293 | 294 => Builtin::DateTime,                // Date, UtcTime
        291 | 292 | 295 | 12877..=12881 | 23751 | 24263 => Builtin::String, // NumericRange, Time, LocaleId, …String, UriString, SemanticVersionString
        30 | 311 | 521 | 2000..=2003 | 16307 => Builtin::ByteString, // Image*, ApplicationInstanceCertificate, ContinuationPoint, AudioDataType
        388 => Builtin::NodeId,     // SessionAuthenticationToken
        11737 => Builtin::UInt64,   // BitFieldMaskDataType
        _ => return None,
    };
    Some(b)
}
