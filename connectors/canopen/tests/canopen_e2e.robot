*** Settings ***
Documentation       End-to-end tests for the tedge-dot CANopen connector against a virtual
...                 CAN interface (vcan0). The connector polls SDO objects from the python-canopen
...                 simulator node and publishes samples to a local MQTT broker; these tests
...                 assert on that output. No cloud (Cumulocity) is involved.
...
...                 Simulator seeds (node ID 1 on vcan0):
...                   0x2000:0  analog_in    = 1234   (uint16, read-only)
...                   0x2001:0  temperature  = -100   (int16,  read-only)
...                   0x2002:0  digital_out  = 1      (uint8,  read-write)
...
...                 Run via:  just test-e2e canopen

Resource            ../../_shared/stack.resource
Library             Collections

Suite Setup         Setup OT Stack    canopen    broker_port=13884
Suite Teardown      Teardown OT Stack


*** Variables ***
${DEVICE}               plc1
${PROTOCOL}             canopen
${SERVICE}              tedge-dot

${SAMPLE_PREFIX}        te/device/${DEVICE}/ot/${PROTOCOL}/sample
${CMD_PREFIX}           te/device/${DEVICE}/ot/${PROTOCOL}/cmd/write
${LINK_TOPIC}           te/device/${DEVICE}/ot/${PROTOCOL}/status/link
${CAPS_TOPIC}           te/device/main/service/${SERVICE}/ot/capabilities
${HEALTH_TOPIC}         te/device/main/service/${SERVICE}/status/health

# CANopen is poll-based (1 s interval); allow a generous startup timeout.
${READY_TIMEOUT}        90
${SAMPLE_TIMEOUT}       10
# Measurement output (connector-measure.toml, openspec native-batched-output): the `com` device.
${MEASURE_TOPIC}            te/device/com///m/${PROTOCOL}
${MEASURE_SAMPLE_PREFIX}    te/device/com/ot/${PROTOCOL}/sample


*** Test Cases ***
B1 Connector Publishes Capability Descriptor
    [Documentation]    The connector advertises its protocol and supported verbs (retained).
    ${payload}=    Wait For Retained    ${CAPS_TOPIC}    timeout=${READY_TIMEOUT}
    ${protocol}=    Get Json Field    ${payload}    protocol
    Should Be Equal    ${protocol}    canopen
    ${verbs}=    Get Json Field    ${payload}    command_verbs
    List Should Contain Value    ${verbs}    write

B1 Service Health Is Up
    [Documentation]    The connector publishes a retained service health status of "up".
    ${payload}=    Wait For Retained    ${HEALTH_TOPIC}    timeout=${READY_TIMEOUT}
    ${status}=    Get Json Field    ${payload}    status
    Should Be Equal    ${status}    up

B5 Device Link Is Connected
    [Documentation]    The connector reports the CANopen device link as connected after SDO probe.
    ${payload}=    Wait For Retained    ${LINK_TOPIC}    timeout=${READY_TIMEOUT}
    ${status}=    Get Json Field    ${payload}    status
    Should Be Equal    ${status}    connected

B2 Reads Analog In As Uint16
    [Documentation]    Reads analog_in (0x2000:0 uint16), seeded to 1234.
    ${payload}=    Wait For Sample    ${SAMPLE_PREFIX}/analog_in    timeout=${SAMPLE_TIMEOUT}
    Sample Should Be Good    ${payload}
    ${datatype}=    Get Json Field    ${payload}    datatype
    Should Be Equal    ${datatype}    uint16
    ${value}=    Get Json Field    ${payload}    value
    Should Be Equal As Numbers    ${value}    1234

B2 Reads Temperature As Int16
    [Documentation]    Reads temperature (0x2001:0 int16), seeded to -100.
    ${payload}=    Wait For Sample    ${SAMPLE_PREFIX}/temperature    timeout=${SAMPLE_TIMEOUT}
    Sample Should Be Good    ${payload}
    ${datatype}=    Get Json Field    ${payload}    datatype
    Should Be Equal    ${datatype}    int16
    ${value}=    Get Json Field    ${payload}    value
    Should Be Equal As Numbers    ${value}    -100

B2 Reads Digital Out As Uint8
    [Documentation]    Reads digital_out (0x2002:0 uint8), seeded to 1.
    ${payload}=    Wait For Sample    ${SAMPLE_PREFIX}/digital_out    timeout=${SAMPLE_TIMEOUT}
    Sample Should Be Good    ${payload}
    ${datatype}=    Get Json Field    ${payload}    datatype
    Should Be Equal    ${datatype}    uint8
    ${value}=    Get Json Field    ${payload}    value
    Should Be Equal As Numbers    ${value}    1

B3 Typed Mode Yields Value And Value Repr
    [Documentation]    Typed-mode samples carry both 'value' and 'value_repr'.
    ${payload}=    Wait For Sample    ${SAMPLE_PREFIX}/analog_in    timeout=${SAMPLE_TIMEOUT}
    ${mode}=    Get Json Field    ${payload}    mode
    Should Be Equal    ${mode}    typed
    ${repr}=    Get Json Field    ${payload}    value_repr
    Should Not Be Empty    ${repr}

B6 Write Command Succeeds
    [Documentation]    A write command for digital_out transitions executing -> successful.
    Publish Message    ${CMD_PREFIX}/dout-1
    ...    {"status":"init","point":"digital_out","value":0}
    ...    retain=True
    ${result}=    Wait For Message Containing
    ...    ${CMD_PREFIX}/dout-1
    ...    "status":"successful"
    ...    timeout=${SAMPLE_TIMEOUT}
    ${point}=    Get Json Field    ${result}    point
    Should Be Equal    ${point}    digital_out

B7 Write To Read-Only Point Fails
    [Documentation]    A write to analog_in (access=read) returns failed with a reason.
    Publish Message    ${CMD_PREFIX}/ain-1
    ...    {"status":"init","point":"analog_in","value":9999}
    ...    retain=True
    ${result}=    Wait For Message Containing
    ...    ${CMD_PREFIX}/ain-1
    ...    "status":"failed"
    ...    timeout=${SAMPLE_TIMEOUT}
    ${reason}=    Get Json Field    ${result}    reason
    Should Not Be Empty    ${reason}

B9 Capability Honesty — No Undeclared Datatypes
    [Documentation]    Every sample's datatype is listed in the capability descriptor.
    ${caps_raw}=    Wait For Retained    ${CAPS_TOPIC}    timeout=${READY_TIMEOUT}
    ${declared}=    Get Json Field    ${caps_raw}    datatypes

    ${payload}=    Wait For Sample    ${SAMPLE_PREFIX}/analog_in    timeout=${SAMPLE_TIMEOUT}
    ${dt}=    Get Json Field    ${payload}    datatype
    List Should Contain Value    ${declared}    ${dt}

B10 Topic Discipline — No Stray Measurement Topics
    [Documentation]    The connector must not publish to te/.../m/ topics.
    No Messages On Topic    te/device/${DEVICE}/m/#    timeout=3


Measurement Output Batches A Polled Pass Into One Measurement
    [Documentation]    In measurement output the connector publishes the thin-edge measurement
    ...                itself, the device's polled points of one group in one message with one
    ...                read time (contract §5.4, §5.5).
    ${payload}=    Wait For Message Containing    ${MEASURE_TOPIC}    "analog_in"    timeout=${READY_TIMEOUT}
    ${analog}=    Get Json Field    ${payload}    canopen.analog_in
    Should Be Equal As Numbers    ${analog}    1234

Measurement Output Names Series As The Measurement Flow Does
    [Documentation]    A point's meta.measurement names its group and series.
    ${payload}=    Wait For Message Containing    te/device/com///m/Node    "Temperature"    timeout=${READY_TIMEOUT}
    ${temperature}=    Get Json Field    ${payload}    Node.Temperature
    Should Be Equal As Numbers    ${temperature}    -100

Measurement Output Publishes The Units Metadata
    [Documentation]    The connector publishes the units of a measurement topic, retained, on
    ...                <topic>/meta: only the series with a unit.
    # Read from the broker: a retained message is delivered once per subscription.
    Wait For Message Containing    ${MEASURE_TOPIC}    "time"    timeout=${READY_TIMEOUT}
    ${payload}=    Fetch Retained    ${MEASURE_TOPIC}/meta    timeout=10
    # The keys hold a dot ("<group>.<series>"), so they are matched as text.
    Should Contain    ${payload}    "canopen.analog_in":{"unit":"counts"}

Measurement Output Publishes No Sample For A Plain Point
    [Documentation]    A point no flow reads publishes no sample in measurement output.
    Wait For Message Containing    ${MEASURE_TOPIC}    "time"    timeout=${READY_TIMEOUT}
    No New Messages On Topic    ${MEASURE_SAMPLE_PREFIX}/analog_in    timeout=5


*** Keywords ***
Sample Should Be Good
    [Arguments]    ${payload}
    ${quality}=    Get Json Field    ${payload}    quality
    Should Be Equal    ${quality}    good
    ${mode}=    Get Json Field    ${payload}    mode
    Should Be Equal    ${mode}    typed
