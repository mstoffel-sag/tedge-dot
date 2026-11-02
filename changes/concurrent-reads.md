### Devices read in parallel

A connector now reads its devices in parallel, and sends up to four Reads at once to an OPC UA
server, in both builds. A connector used to read one device after another, one request at a
time, so its throughput was set by the PLCs' round-trip times: with 20 ms per read it stopped at
about 22,000 readings/s (16,500 in the C build) while its CPU idled. Measured on the same host,
one connector now reads 64,000 readings/s from 64 devices in both builds, and one OPC UA device
with 20,000 nodes about 64,000 readings/s with the default budget, 80,000 with
`max_inflight = 8`. See [doc/concurrent-reads.md](../doc/concurrent-reads.md).

- New `max_inflight` (default 4) on `[connector]` or a device: requests in flight to one
  endpoint (a host and port, a serial port or a CAN bus), shared by every device on it. A bus
  always carries one request at a time. `max_inflight_total` (default 32) bounds all endpoints.
- A server that answers busy (`BadServerTooBusy`) gets fewer requests, and the refused one is
  sent again.
- **Behavior change:** devices on different endpoints are read in parallel, and OPC UA devices
  get up to four Reads at once. `max_inflight = 1` sends one request at a time to each endpoint.
- Polls now run on their interval's grid, so a 1 s poll no longer runs every 1.0 to 1.2 s.
- The capability descriptor lists the endpoints (`endpoints`). The OT Connector Contract is now
  version 0.4.0 (§5.6).
