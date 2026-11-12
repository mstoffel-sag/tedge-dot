// Short help for every input field of the tools, shown in a popup next to the field's label
// (src/components/tools/Help.tsx). A test holds the config builder's texts to the contract schema
// and the builder's protocol shapes, so a new key gets a text.
//
// Keys: "<section>.<key>" for the configuration (connector, mqtt, device, point, report, grouping, measurement),
// "<protocol>.<table>.<key>" for protocol-specific tables (protocol_address, address,
// connection), and "workbench.<name>" / "picker.<name>" for the tools' own fields.

export const HELP: Record<string, string> = {
  // [connector]
  'connector.protocol': 'The protocol of every device this connector reads. Switching keeps point ids and policies and gives the points new addresses.',
  'connector.service_name': 'The service name. It addresses the connector\'s commands on the broker, so it must be unique. Default tedge-dot-<protocol>.',
  'connector.poll_interval': 'How often a polled point is read, unless its device or the point sets its own. Default 2s.',
  'connector.sampling_interval': 'How often the OPC UA server samples a subscribed point, unless the device or point sets one. Default: the poll_interval.',
  'connector.log_level': 'How much the service logs: error, warn, info, debug or trace.',
  'connector.output': '"single" publishes each reading as its own sample; "group" puts many series into each thin-edge measurement.',
  'connector.max_inflight': 'Requests the connector keeps in flight to one endpoint at a time.',
  'connector.max_inflight_total': 'Requests the connector keeps in flight across all endpoints at a time.',
  'connector.report': 'The reporting policy of every point that does not set its own: which readings are published.',
  'connector.grouping': 'How grouped output puts readings into measurements, for every point that does not set its own.',
  'connector.measurement': 'How grouped output names the measurements: type, fragment and series.',

  // [mqtt]
  'mqtt.host': 'The MQTT broker the connector publishes to, usually the thin-edge.io broker on the gateway.',
  'mqtt.port': 'The broker port, usually 1883.',

  // [[device]]
  'device.name': "The device's name: its child-device id in thin-edge.io and in the MQTT topics.",
  'device.poll_interval': "How often this device's polled points are read, over [connector]'s.",
  'device.type': 'The device type. It names the parameter sets in Cumulocity, so give identical devices the same type.',
  'device.default_mode': '"typed" decodes values to the datatype; "raw" publishes the bytes as read.',
  'device.max_inflight': 'Requests in flight to this device at a time, over [connector] max_inflight.',
  'device.points_from': 'Point libraries the device takes its points from, in order. Its own points apply over them, by id.',
  'device.report': "This device's reporting policy, merged over [connector]'s.",
  'device.grouping': "This device's grouping in grouped output, merged over [connector]'s.",

  // [[device.point]]
  'point.id': 'The point id: its series name and MQTT topic segment. In the workbench, the signal with this name is what it reads.',
  'point.datatype': 'How the value is decoded, for example float32 or uint16.',
  'point.access': 'What the connector may do with the point: read it, write it, both, or call it as a method.',
  'point.unit': 'The unit Cumulocity shows with the value, for example °C.',
  'point.poll_interval': 'How often this point is read when polled, over its device and [connector].',
  'point.sampling_interval': 'How often the OPC UA server samples this point when subscribed. "0" asks for its fastest rate.',
  'point.subscribe': 'On: the server pushes the value when it changes. Off: the connector polls it every poll_interval.',
  'point.report': "This point's reporting policy, merged key by key over its device's and [connector]'s.",
  'point.grouping': "This point's grouping in grouped output, merged over its device's and [connector]'s.",

  // report
  'report.on_change': 'Publish a reading only when its value differs from the last one published.',
  'report.deadband': 'Publish only when the value moved by more than this, as a number or a percentage like "2%".',
  'report.min_interval': 'Publish at most once per interval. A held reading is published later, with its own timestamp.',
  'report.max_interval': 'Heartbeat: publish at least once per interval, even when nothing changed. "0" switches it off.',
  'report.debounce': 'Publish a change only once the value has stayed for this long.',

  // grouping
  'grouping.by': 'time: readings with the same timestamp share a measurement. frame: readings within a window. sample: every value at a fixed period.',
  'grouping.window': 'frame: readings within this window share a measurement stamped with its first time.',
  'grouping.every': 'sample: the period at which every point sends the value it holds.',
  'grouping.aggregate': 'sample: send the mean, min, max or last of each period instead of the value held at the sample time.',

  // measurement (naming in grouped output)
  'measurement.type': 'The measurement type: the topic m/<type> and the Cumulocity type. Default: the protocol.',
  'measurement.fragment': 'The fragment that holds the series in the measurement body. Default: the type.',
  'measurement.point_separator': 'Split a point id at this text: the part before names the fragment (and the type, if unset), the rest the series.',

  // Modbus
  'modbus.protocol_address.transport': 'tcp for Modbus TCP, rtu for a serial line.',
  'modbus.protocol_address.host': "TCP: the device's IP address or host name.",
  'modbus.protocol_address.port': 'TCP: the device port, usually 502.',
  'modbus.protocol_address.unit_id': 'The Modbus unit (slave) id of the device.',
  'modbus.protocol_address.serial_port': 'RTU: the serial device, for example /dev/ttyRS485.',
  'modbus.protocol_address.baudrate': 'RTU: the line speed, over [connection.serial].',
  'modbus.protocol_address.parity': 'RTU: N (none), E (even) or O (odd), over [connection.serial].',
  'modbus.protocol_address.stopbits': 'RTU: 1 or 2 stop bits, over [connection.serial].',
  'modbus.protocol_address.databits': 'RTU: data bits per character, usually 8, over [connection.serial].',
  'modbus.address.table': 'The register table: coil, discrete_input, holding or input.',
  'modbus.address.address': 'The 0-based register or coil address.',
  'modbus.address.count': 'How many registers the value spans, for example 2 for a float32.',
  'modbus.address.start_bit': 'For a bit field: the 0-based first bit within the registers.',
  'modbus.address.bit_count': 'For a bit field: how many bits it has.',
  'modbus.connection.serial': 'RTU defaults (baudrate, parity, stopbits, databits) for devices that do not set them.',

  // OPC UA
  'opcua.protocol_address.endpoint': 'The server endpoint, for example opc.tcp://192.168.0.20:4840/.',
  'opcua.protocol_address.security_policy': "The security policy for this server, over [connection]'s, for example Basic256Sha256.",
  'opcua.protocol_address.security_mode': "None, Sign or SignAndEncrypt, over [connection]'s.",
  'opcua.protocol_address.user': 'The user name to log in with.',
  'opcua.protocol_address.password': 'The password to log in with. Prefer password_file, so the secret is not in the config.',
  'opcua.protocol_address.password_file': 'A file holding the password, relative to the config file.',
  'opcua.protocol_address.user_certificate': 'A user certificate to log in with instead of a password.',
  'opcua.protocol_address.user_private_key': "The private key of user_certificate.",
  'opcua.protocol_address.allow_plaintext_password': 'Allow the password to travel unencrypted. Off by default.',
  'opcua.protocol_address.allow_deprecated_security': 'Allow deprecated policies such as Basic128Rsa15. Off by default.',
  'opcua.protocol_address.trust_any_server_certificate': "Accept any server certificate without checking it. For testing only.",
  'opcua.protocol_address.namespaces': "The namespace table the device's node ids were written against, so ns= indexes stay right if the server reorders them.",
  'opcua.address.node_id': 'The node to read, for example ns=2;s=Temperature.',
  'opcua.address.namespace': 'The namespace index, when the node is given as namespace and identifier.',
  'opcua.address.identifier': 'The node identifier within the namespace.',
  'opcua.address.field': 'A dotted path into a structured value, for example Motor.Current.',
  'opcua.address.index': 'One zero-based element of an array value.',
  'opcua.connection.application_name': "The client's application name, shown to the server.",
  'opcua.connection.application_uri': "The client's application URI. It must be in the application certificate.",
  'opcua.connection.security_policy': 'The default security policy of every device.',
  'opcua.connection.security_mode': 'The default security mode of every device: None, Sign or SignAndEncrypt.',
  'opcua.connection.connect_timeout_s': 'Seconds allowed for discovery and session activation. Default 15.',
  'opcua.connection.request_timeout_s': 'Seconds allowed for one service call. Default: [connector] operation_timeout.',

  // CAN bus
  'canbus.protocol_address.interface': 'The SocketCAN interface, for example can0.',
  'canbus.protocol_address.dbc_file': 'The DBC file that describes the messages and signals.',
  'canbus.address.message_name': 'The CAN message, by its name in the DBC file.',
  'canbus.address.signal_name': 'The signal within the message, by its name in the DBC file.',

  // CANopen
  'canopen.protocol_address.node_id': 'The CANopen node id of the device (1 to 127).',
  'canopen.address.index': 'The object dictionary index, for example 24641 (0x6041).',
  'canopen.address.subindex': 'The object dictionary subindex.',
  'canopen.connection.interface': 'The SocketCAN interface, for example can0.',

  // SNMP
  'snmp.protocol_address.host': "The agent's IP address or host name.",
  'snmp.protocol_address.port': 'The agent port, usually 161.',
  'snmp.protocol_address.version': 'The SNMP version: v1, v2c or v3.',
  'snmp.protocol_address.community': 'v1 and v2c: the community used to read.',
  'snmp.protocol_address.write_community': 'v1 and v2c: the community used to write (SET). Default: community.',
  'snmp.address.oid': 'The object to read, for example 1.3.6.1.2.1.1.3.0.',
  'snmp.address.trap': 'Receive notifications with this trap OID, or beneath it, instead of reading an OID.',
  'snmp.connection.community': 'The default community of every device.',
  'snmp.connection.listen': 'The UDP address traps are received on, for example 0.0.0.0:162.',

  // PROFIBUS
  'profibus.protocol_address.station_address': 'The slave station address on the bus.',
  'profibus.protocol_address.ident_number': "The slave's PROFIBUS ident number, from its GSD file.",
  'profibus.protocol_address.input_bytes': 'How many input bytes the slave sends in each cycle.',
  'profibus.protocol_address.output_bytes': 'How many output bytes the slave receives in each cycle.',
  'profibus.address.direction': 'input (from the slave) or output (to the slave).',
  'profibus.address.byte_offset': 'The 0-based byte in the input or output data.',
  'profibus.address.bit_offset': 'For a bit value: the 0-based bit within the byte.',
  'profibus.address.bit_count': 'For a bit field: how many bits it has.',
  'profibus.connection.port': 'The bus: a serial-to-TCP device server as tcp://host:port.',
  'profibus.connection.baudrate': 'The bus speed in bit/s, for example 19200.',
  'profibus.connection.master_address': "The gateway's own station address on the bus.",
  'profibus.connection.slot_bits': 'The slot time in bit times: how long the master waits for an answer.',

  // AVEVA PI Web API
  'aveva-pi.protocol_address.base_url': 'The PI Web API root, for example https://pi.example.com/piwebapi. http:// only without auth.',
  'aveva-pi.protocol_address.data_server': 'The PI Data Archive that tag points live in.',
  'aveva-pi.protocol_address.af_database': 'The AF database of relative attribute paths, as \\\\<AF server>\\<database>.',
  'aveva-pi.protocol_address.delivery': 'channel (pushed, the default), recorded (archive polled) or snapshot (current value polled).',
  'aveva-pi.address.tag': "A PI point name on the device's data_server.",
  'aveva-pi.address.path': 'A full PI point path, or an AF attribute path with | before the attribute name.',
  'aveva-pi.address.web_id': 'A PI Web API WebId. It is used as given, without a lookup.',
  'aveva-pi.connection.auth': 'none, basic, bearer or certificate. Secrets are read from files (password_file, token_file, key_file).',
  'aveva-pi.connection.tls': 'ca_file: a CA bundle for the server certificate. insecure_skip_verify turns the check off.',

  // The workbench's own fields
  'workbench.example': 'Replace the signals and the configuration with a ready-made case.',
  'workbench.demo': 'Load a demo config from the repository, wired to the demo simulators.',
  'workbench.paste': 'Load your own configuration. The signals follow: one per point.',
  'workbench.signal': 'The signal name. The config point with the same id reads it.',
  'workbench.behaviour': 'How the value moves over the run. The unit becomes the point unit.',
  'workbench.outage': 'A part of the run in which every read of this signal fails (quality "bad").',
  'workbench.duration': 'How long the simulation runs, for every point and the grouping view.',
  'workbench.span': 'How much of the run the grouping charts and the table show.',
  'workbench.fragments': 'How the series are split into fragments of the measurement. Several fragments of one type share a measurement.',
  'workbench.fragment_writes': 'The fragments in all measurements. Streaming Lake Ingestion writes each fragment of a measurement separately.',
  'workbench.from': 'Where the part of the run that is shown starts.',

  // The package picker
  'picker.architecture': "The gateway's CPU. uname -m prints it.",
  'picker.distribution': "The gateway's operating system. It decides the glibc version a package needs.",
  'picker.protocols': 'The protocols your devices speak.',
  'picker.features': 'Features only one of the packages has.',
  'picker.footprint': 'Choose this when a few MB of disk or memory matter.',
};

/** The help for a configuration field at `path` (a JSON pointer), for `protocol`. */
export function helpForPath(path: string, protocol: string): string | undefined {
  const keys = path.split('/').filter((k) => k && !/^\d+$/.test(k));
  const key = keys.at(-1) ?? '';
  const table = keys.at(-2) ?? '';
  if (['protocol_address', 'address', 'connection'].includes(table)) return HELP[`${protocol}.${table}.${key}`];
  if (table === 'report' || table === 'grouping' || table === 'measurement') return HELP[`${table}.${key}`];
  // The deprecated `batch` table is `grouping`, with `group_by` named `by`.
  if (table === 'batch') return HELP[`grouping.${key === 'group_by' ? 'by' : key}`];
  if (table === 'point') return HELP[`point.${key}`];
  if (table === 'device') return HELP[`device.${key}`];
  return HELP[`${table}.${key}`];
}
