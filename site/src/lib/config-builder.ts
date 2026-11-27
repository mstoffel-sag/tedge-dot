// The config builder's model: a connector configuration is kept as the plain object TOML
// parses to, validated with ajv against the contract schema (doc/contract/schemas/
// config.schema.json) plus the builder's protocol-specific shapes (src/data/protocols.json),
// and emitted as TOML laid out the way the shipped configs are.

import Ajv2020 from 'ajv/dist/2020';
import type { ErrorObject, ValidateFunction } from 'ajv';
import { parse, stringify } from 'smol-toml';
import configSchema from '../../../doc/contract/schemas/config.schema.json';
import librarySchema from '../../../doc/contract/schemas/point-library.schema.json';
import protocols from '../data/protocols.json';

export type Config = Record<string, any>;
export type Protocols = typeof protocols;
export type ProtocolId = Exclude<keyof Protocols, '$comment'>;

export const PROTOCOL_IDS = Object.keys(protocols).filter((k) => k !== '$comment') as ProtocolId[];

export interface FieldError {
  /** JSON pointer of the value, e.g. /device/0/protocol_address/port */
  path: string;
  message: string;
}

const ajv = new Ajv2020({ allErrors: true, strict: false });
// The point-library schema refers to the config schema's point definition by $id.
ajv.addSchema(configSchema);
ajv.addSchema(librarySchema);

function overlay(protocol: string, forLibrary: boolean) {
  const p = (protocols as any)[protocol];
  if (!p) return {};
  // Rules that tie a point's keys together, such as OPC UA's method addressing.
  const point = { properties: { address: p.address }, ...(p.pointRules ? { allOf: p.pointRules } : {}) };
  if (forLibrary) return { properties: { point: { items: point } } };
  return {
    properties: {
      connection: p.connection,
      device: {
        items: {
          properties: { protocol_address: p.protocol_address, point: { items: point } },
        },
      },
    },
  };
}

const cache = new Map<string, ValidateFunction>();
function validator(protocol: string, forLibrary = false): ValidateFunction {
  const key = `${forLibrary ? 'lib' : 'cfg'}:${protocol}`;
  let v = cache.get(key);
  if (!v) {
    const base = (forLibrary ? librarySchema : configSchema).$id;
    v = ajv.compile({ allOf: [{ $ref: base }, overlay(protocol, forLibrary)] });
    cache.set(key, v);
  }
  return v;
}

/** Validate a configuration; errors carry the pointer of the value they concern. */
export function validateConfig(config: Config): FieldError[] {
  const protocol = config?.connector?.protocol ?? '';
  const v = validator(protocol);
  return v(config) ? [] : toFieldErrors(v.errors ?? []);
}

/** Validate a point library (demo/points.d). */
export function validateLibrary(library: Config): FieldError[] {
  const v = validator(library?.library?.protocol ?? '', true);
  return v(library) ? [] : toFieldErrors(v.errors ?? []);
}

/** Address keys that only a method point has (OPC UA, connector spec §3.10). */
const METHOD_ADDRESS_KEYS = ['object_id', 'method_id', 'inputs'];

function toFieldErrors(errors: ErrorObject[]): FieldError[] {
  const out: FieldError[] = [];
  const seen = new Set<string>();
  for (const e of errors) {
    // `if`/`allOf`/`oneOf` wrappers repeat what a concrete error already says.
    if (e.keyword === 'if' || e.keyword === 'allOf') continue;
    let path = e.instancePath;
    let message = e.message ?? 'is invalid';
    if (e.keyword === 'required') {
      path = `${path}/${(e.params as any).missingProperty}`;
      message = 'is required';
    } else if (e.keyword === 'additionalProperties') {
      path = `${path}/${(e.params as any).additionalProperty}`;
      message = 'is not a known key here';
    } else if (e.keyword === 'false schema') {
      // Only the call_method rules use `false`: the contract schema's value fields and the
      // protocol's address keys. The messages are the connectors' own.
      const key = path.split('/').pop() ?? '';
      message = METHOD_ADDRESS_KEYS.includes(key) ? 'applies only to a call_method point' : 'does not apply to a call_method point';
    } else if (e.keyword === 'enum') {
      message = `must be one of ${(e.params as any).allowedValues.map((v: unknown) => JSON.stringify(v)).join(', ')}`;
    }
    const key = `${path}|${message}`;
    if (seen.has(key)) continue;
    seen.add(key);
    out.push({ path, message });
  }
  return out;
}

export function parseToml(text: string): Config {
  return parse(text) as Config;
}

// --- TOML output ----------------------------------------------------------------------------

const BARE_KEY = /^[A-Za-z0-9_-]+$/;
const key = (k: string) => (BARE_KEY.test(k) ? k : JSON.stringify(k));

/** A value on one line: primitives as TOML does, tables inline, arrays inline. */
export function inline(value: unknown): string {
  if (Array.isArray(value)) return `[${value.map(inline).join(', ')}]`;
  if (value !== null && typeof value === 'object' && !(value instanceof Date)) {
    const entries = Object.entries(value as Record<string, unknown>).filter(([, v]) => v !== undefined);
    if (!entries.length) return '{}';
    return `{ ${entries.map(([k, v]) => `${key(k)} = ${inline(v)}`).join(', ')} }`;
  }
  // smol-toml formats strings, numbers, booleans and dates correctly.
  return stringify({ v: value }).replace(/^v = /, '').trim();
}

function block(obj: Record<string, unknown>, indent = ''): string[] {
  const entries = Object.entries(obj).filter(([, v]) => v !== undefined);
  const width = Math.max(0, ...entries.map(([k]) => key(k).length));
  return entries.map(([k, v]) => `${indent}${key(k).padEnd(width)} = ${inline(v)}`);
}

const SECTION_ORDER = ['connector', 'mqtt', 'connection'];

/** Emit a configuration as TOML: sections, then one [[device]] per device and its points. */
export function emitToml(config: Config): string {
  const lines: string[] = [];
  const sections = [
    ...SECTION_ORDER.filter((s) => s in config),
    ...Object.keys(config).filter((k) => !SECTION_ORDER.includes(k) && k !== 'device'),
  ];
  for (const name of sections) {
    const value = config[name];
    if (value === undefined) continue;
    if (value === null || typeof value !== 'object' || Array.isArray(value)) {
      lines.unshift(`${key(name)} = ${inline(value)}`);
      continue;
    }
    if (lines.length) lines.push('');
    lines.push(`[${key(name)}]`, ...block(value));
  }
  for (const device of (config.device ?? []) as Record<string, unknown>[]) {
    const { point, ...rest } = device;
    lines.push('', '[[device]]', ...block(rest));
    for (const p of (point ?? []) as Record<string, unknown>[]) {
      lines.push('', '  [[device.point]]', ...block(p, '  '));
    }
  }
  return lines.join('\n') + '\n';
}

// --- editing helpers --------------------------------------------------------------------------

/** Read the value at a JSON pointer. */
export function getAt(obj: any, path: string): any {
  return path
    .split('/')
    .slice(1)
    .reduce((o, k) => (o == null ? undefined : o[k]), obj);
}

/** A copy of `obj` with the value at `path` set (or removed when `value` is undefined or ''). */
export function setAt(obj: any, path: string, value: unknown): any {
  const keys = path.split('/').slice(1);
  const copy = (o: any, i: number): any => {
    const k = keys[i];
    const base = Array.isArray(o) ? [...o] : { ...(o ?? {}) };
    if (i === keys.length - 1) {
      if (value === undefined || value === '') delete base[k];
      else base[k] = value;
      return base;
    }
    const next = copy(o?.[k] ?? (/^\d+$/.test(keys[i + 1]) ? [] : {}), i + 1);
    if (next && typeof next === 'object' && !Array.isArray(next) && !Object.keys(next).length) delete base[k];
    else base[k] = next;
    return base;
  };
  return copy(obj, 0);
}

/** The input's text as a value of the type the field expects; text that is not one stays text. */
export function coerce(text: string, type: string | undefined): unknown {
  if (text === '') return undefined;
  if ((type === 'integer' || type === 'number') && /^-?\d+(\.\d+)?$/.test(text.trim())) return Number(text);
  return text;
}

/** A protocol's shapes and templates; a protocol the builder does not know gets generic ones. */
export function protocolInfo(protocol: string): any {
  return (
    (protocols as any)[protocol] ?? {
      label: protocol || 'unknown',
      protocol_address: { type: 'object' },
      address: { type: 'object' },
      connection: { type: 'object' },
      newDevice: { name: 'device1', protocol_address: {} },
      newPoint: { id: 'point1', access: 'read' },
    }
  );
}

export function newConfig(protocol: ProtocolId): Config {
  const p = protocolInfo(protocol);
  return {
    connector: { protocol, service_name: `tedge-dot-${protocol}`, poll_interval: '2s', report: { max_interval: '30m' } },
    mqtt: { host: '127.0.0.1', port: 1883 },
    device: [{ ...structuredClone(p.newDevice), point: [structuredClone(p.newPoint)] }],
  };
}

export const SCHEMA = configSchema as any;
