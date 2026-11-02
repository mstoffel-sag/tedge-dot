### Fewer measurement messages with `group_by = "frame"`

In measurement output, a frame no longer ends when a point changes twice within the window. The
second value goes into a second message of the same window, the third into a third, and all of
them are published when the window ends. A device with 1,000 changing points now publishes a few
messages per window instead of one for every 30 to 40 values, so a longer window now saves
mapper CPU. See [doc/measurement-output.md](../doc/measurement-output.md#frame-a-window-of-the-device-clock).

- Nothing to configure. The examples now use `window = "500ms"`, which suits most pushed
  telemetry.
- **Behavior change for existing `frame` users:** a repeated point's earlier value is published
  at the end of the window instead of at once, still within the window plus one pass.
- Message times of one measurement topic now always increase. When source times jitter, a
  message can be stamped 1 ms after the previous one instead of with its first reading's time, so
  a point's values reach Cumulocity in the order they were read.
- The OT Connector Contract is now version 0.2.1 (§5.5).
