## MODIFIED Requirements

### Requirement: Group by frame
Under `group_by = "frame"`, the readings that arrive within one window of the device clock SHALL
be published as the frame's **layers**. A reading SHALL go into the first layer that does not
hold its point yet: a point's first value in the window goes into layer 0, its second into
layer 1, and so on. No reading SHALL be dropped. The layers SHALL be published when the window
ends, in layer order, and a reading that arrives after the window ended SHALL open a new window
even when the ended one is not published yet. A reading that would open a 65th layer SHALL
publish the frame's layers at once and start a new frame in the same window. A layer larger than
the size budget SHALL be split, each part keeping the layer's time.

A layer's time SHALL be its first reading's time, unless that is not later than the time of the
previous layer of the same topic and grouping policy, in this frame or an earlier one. In that
case it SHALL be 1 ms after that previous time, so layer times of one topic and policy strictly
increase; the parts of a split layer share its time. Apart from that move, a reading's published
time SHALL differ from its own timestamp by less than the window.

#### Scenario: Frame groups pushed readings
- **WHEN** `window = "100ms"` and 400 pushed readings of distinct points arrive within one window
- **THEN** they are published together, stamped with the first reading's time, within 100 ms after the window opened plus one runtime pass

#### Scenario: A repeat goes into the next layer
- **WHEN** readings of points A, B, A and C arrive in that order within one window
- **THEN** at the end of the window the connector publishes one message holding A's first value, B and C, then one message holding A's second value

#### Scenario: Nothing overwritten
- **WHEN** a point changes three times within one window
- **THEN** all three values are published, in three messages, in the order they arrived

#### Scenario: Times stay in order when source times jitter
- **WHEN** layer 0 of a frame is stamped 12:00:00.140 and the first reading of layer 1 carries the source time 12:00:00.125
- **THEN** layer 1 is stamped 12:00:00.141, and every point's values reach the cloud in the order they arrived

#### Scenario: Polled passes keep their read times
- **WHEN** a device is polled every 200 ms under `window = "500ms"`, each pass sharing one read time
- **THEN** each pass after the first in the window goes into its own layer, stamped with its own read time

#### Scenario: Layer cap
- **WHEN** one point changes 65 times within one window
- **THEN** the 65th reading publishes the frame's 64 layers at once and opens a new frame in the same window, and no value is lost
