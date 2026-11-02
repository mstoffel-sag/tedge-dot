*** Settings ***
Documentation       End-to-end tests for the tedge-dot SNMP connector: polling (GET and GETBULK), SET
...                 writes, and notifications (traps and informs, v1/v2c/v3, direct and through a
...                 trusted forwarder). The pysnmp `agent*` containers are the polled devices (seeds
...                 in connectors/snmp/agent/agent.py); net-snmp in `simulator` sends notifications
...                 (connectors/snmp/sim/send-trap); `stranger` is configured as no device;
...                 `forwarded` sends through the snmptrapd `forwarder`. The flows runner turns
...                 notifications into events and alarms. No cloud (Cumulocity) is involved.
...
...                 Every notification is triggered by the test that asserts on it, and each wait
...                 only accepts messages that arrived after that trigger.
...
...                 Run via:  just test-e2e snmp   /   just test-e2e-c snmp

Resource            ../../_shared/stack.resource
Library             Collections
Library             String

Suite Setup         Setup SNMP Stack
Suite Teardown      Teardown OT Stack


*** Variables ***
${DEVICE}               switch
${PROTOCOL}             snmp
${SERVICE}              tedge-dot

${SAMPLE_PREFIX}        te/device/${DEVICE}/ot/${PROTOCOL}/sample
${LINK_TOPIC}           te/device/${DEVICE}/ot/${PROTOCOL}/status/link
${CAPS_TOPIC}           te/device/main/service/${SERVICE}/ot/capabilities
${HEALTH_TOPIC}         te/device/main/service/${SERVICE}/status/health
${EVENT_TOPIC}          te/device/${DEVICE}///e/link_down
${ALARM_TOPIC}          te/device/${DEVICE}///a/link_down

${LINK_DOWN}            1.3.6.1.6.3.1.1.5.3
${LINK_UP}              1.3.6.1.6.3.1.1.5.4

# Seeds of the agent (connectors/snmp/agent/agent.py).
${E}                    1.3.6.1.4.1.99999.1
${SYS_DESCR}            tedge-dot SNMP agent simulator
${TEXT_UTF8}            Pump station 7 – Zürich
${COUNTER64}            9007199254740993

# Every secret in connector.toml, send-trap and connectors/snmp/secrets/.
@{SECRETS}              trap-auth-pass-1    trap-priv-pass-1    ro-auth-pass-1    rw-auth-pass-1
...                     rw-priv-pass-1    sha2-auth-pass-1    sha2-priv-pass-1    wrong-password-9

${READY_TIMEOUT}        90
# Measurement output (connector-measure.toml, openspec native-batched-output): the `snmpm` device.
${MEASURE_TOPIC}            te/device/snmpm///m/${PROTOCOL}
${MEASURE_SAMPLE_PREFIX}    te/device/snmpm/ot/${PROTOCOL}/sample
# The flows container installs thin-edge from the main channel at build time; give it time.
${MEASURE_FLOWS_TIMEOUT}    120
${SAMPLE_TIMEOUT}       10
# request_timeout 1s x (1 + retries 1), then the runtime's reconnect backoff.
${OUTAGE_TIMEOUT}       30
${RECOVERY_TIMEOUT}     60


*** Test Cases ***
Connector Publishes Capability Descriptor
    [Documentation]    Polling, notifications, GETBULK and SNMPv3, and writable object points.
    ${payload}=    Wait For Retained    ${CAPS_TOPIC}    timeout=${READY_TIMEOUT}
    ${protocol}=    Get Json Field    ${payload}    protocol
    Should Be Equal    ${protocol}    snmp
    ${subscribe}=    Get Json Field    ${payload}    subscribe
    Should Be True    ${subscribe}
    ${features}=    Get Json Field    ${payload}    features
    FOR    ${feature}    IN    polling    subscribe    bulk_read    snmpv3
        List Should Contain Value    ${features}    ${feature}
    END
    ${kinds}=    Get Json Field    ${payload}    point_kinds
    FOR    ${kind}    IN    object    trap    varbind
        List Should Contain Value    ${kinds}    ${kind}
    END
    ${verbs}=    Get Json Field    ${payload}    command_verbs
    List Should Contain Value    ${verbs}    write
    List Should Contain Value    ${verbs}    write-batch

Service Health Is Up
    ${payload}=    Wait For Retained    ${HEALTH_TOPIC}    timeout=${READY_TIMEOUT}
    ${status}=    Get Json Field    ${payload}    status
    Should Be Equal    ${status}    up

Device Link Is Connected Once Its Host Resolves
    [Documentation]    The device host is the simulator's DNS name, resolved on connect.
    ${payload}=    Wait For Fresh Message With Field    ${LINK_TOPIC}    status    connected
    ...    timeout=${READY_TIMEOUT}    since=0
    ${host}=    Get Json Field    ${payload}    info.host
    Should Be Equal    ${host}    simulator
    ${points}=    Get Json Field    ${payload}    points
    List Should Contain Value    ${points}    link_down

Link Info Names Host Port And Version Only
    [Documentation]    `info` is exactly { host, port, version }: no community, user or key.
    Link Info Should Be    agent    agent    161    v2c
    Link Info Should Be    agent-v1    agent-v1    161    v1
    Link Info Should Be    agent-v3-priv    agent-v3-priv    161    v3

# --- polling -------------------------------------------------------------------------------

Reads Strings, Integers And Addresses
    ${sample}=    Good Sample    agent    sys_descr
    Json Field Should Be    ${sample}    value    ${SYS_DESCR}
    Json Field Should Be    ${sample}    addr.oid    1.3.6.1.2.1.1.1.0
    Good Sample Value Should Be    agent    int_negative    -42
    Good Sample Value Should Be    agent    int_seed    1234
    ${sample}=    Good Sample    agent    text_utf8
    Json Field Should Be    ${sample}    value    ${TEXT_UTF8}
    ${sample}=    Good Sample    agent    oid_value
    Json Field Should Be    ${sample}    value    1.3.6.1.4.1.99999.42.7
    ${sample}=    Good Sample    agent    ip_value
    Json Field Should Be    ${sample}    value    192.168.10.2

Reads Counters, Gauges And TimeTicks
    Good Sample Value Should Be    agent    counter32    4000000000
    Good Sample Value Should Be    agent    gauge_float    42
    ${sample}=    Good Sample    agent    gauge_bool
    Json Field Should Be    ${sample}    value    ${True}
    Good Sample Value Should Be    agent    timeticks    123456
    ${sample}=    Good Sample    agent    sys_uptime
    ${uptime}=    Get Json Field    ${sample}    value
    Should Be True    ${uptime} > 0

A Counter64 Beyond 2^53 Is Published As A Decimal String
    ${sample}=    Good Sample    agent    counter64
    ${value}=    Get Json Field    ${sample}    value
    ${is_string}=    Evaluate    isinstance($value, str)
    Should Be True    ${is_string}    msg=Counter64 ${value} was not published as a string
    Should Be Equal    ${value}    ${COUNTER64}

A Changing Value Keeps Changing
    [Documentation]    The agent's seconds counter, polled every second.
    ${first}=    Good Sample Value    agent    ticks
    Sleep    2.5s
    ${second}=    Good Sample Value    agent    ticks
    Sleep    2.5s
    ${third}=    Good Sample Value    agent    ticks
    Should Be True    ${first} < ${second} < ${third}    msg=ticks did not keep increasing: ${first}, ${second}, ${third}

A Value That Does Not Convert Is Bad And Keeps Its Raw Octets
    ${sample}=    Bad Sample    agent    binary_string
    Json Field Should Be    ${sample}    raw    de ad be ef 00 ff
    Bad Sample    agent    counter32_as_int32
    ${sample}=    Bad Sample    agent    opaque_typed
    Json Field Should Be    ${sample}    raw    01 02 03 04

Raw Mode Carries The Content Octets
    ${sample}=    Good Sample    agent    binary_raw
    Json Field Should Be    ${sample}    mode    raw
    Json Field Should Be    ${sample}    raw    de ad be ef 00 ff
    ${sample}=    Good Sample    agent    opaque_raw
    Json Field Should Be    ${sample}    raw    01 02 03 04

An Unserved OID Is A Bad Sample Beside Good Ones
    [Documentation]    noSuchObject and noSuchInstance are per-varbind exceptions: bad samples for
    ...    those points only, in the same request as good ones.
    ${sample}=    Bad Sample    agent    missing_object
    ${error}=    Get Json Field    ${sample}    error
    Should Not Be Empty    ${error}
    Bad Sample    agent    missing_instance
    Good Sample    agent    int_seed

A v1 noSuchName Fails Only The Point It Names
    [Documentation]    SNMPv1 answers an unserved OID with error-status noSuchName for the whole
    ...    request; the connector marks that point bad and re-requests the others.
    ${mark}=    Get Message Mark
    ${sample}=    Wait For Fresh Message With Field    te/device/agent-v1/ot/${PROTOCOL}/sample/missing_object
    ...    quality    bad    timeout=${SAMPLE_TIMEOUT}    since=${mark}
    ${error}=    Get Json Field    ${sample}    error
    Should Not Be Empty    ${error}
    FOR    ${point}    IN    sys_descr    int_seed    ticks
        Wait For Fresh Message With Field    te/device/agent-v1/ot/${PROTOCOL}/sample/${point}    quality    good
        ...    timeout=${SAMPLE_TIMEOUT}    since=${mark}
    END
    Good Sample Value Should Be    agent-v1    int_seed    1234

GET And GETBULK Devices Both Read
    [Documentation]    `agent` (v2c) uses GETBULK by default; `agent-v3-auth` sets `bulk = false`
    ...    and `agent-v1` cannot do anything but GET. Proven from the agents' own request logs.
    Good Sample    agent    sys_descr
    Good Sample    agent-v3-auth    sys_descr
    Good Sample    agent-v1    sys_descr
    Agent Should Have Received    agent    v2c GetBulkRequestPDU
    Agent Should Have Received    agent-v3-auth    v3 GetRequestPDU
    Agent Should Not Have Received    agent-v3-auth    GetBulkRequestPDU
    Agent Should Have Received    agent-v1    v1 GetRequestPDU

# --- writes --------------------------------------------------------------------------------

Writes An Integer And Reads It Back
    ${result}=    Write Point    agent    setpoint    55    set-int-1
    Json Field Should Be    ${result}    status    successful
    Fresh Sample Value Should Be    agent    setpoint    55
    Agent Value Should Be    agent    ${E}.20.0    INTEGER: 55

Writes A String And Reads It Back
    ${result}=    Write Point    agent    label    "written by tedge-dot"    set-str-1
    Json Field Should Be    ${result}    status    successful
    ${sample}=    Wait For Fresh Message With Field    te/device/agent/ot/${PROTOCOL}/sample/label
    ...    value    written by tedge-dot    timeout=${SAMPLE_TIMEOUT}
    Agent Value Should Be    agent    ${E}.21.0    STRING: "written by tedge-dot"

A Write Uses The Configured SNMP Type
    [Documentation]    `limit` is uint32 with `type = "gauge32"`: the SET carries a Gauge32, which
    ...    the agent (a Gauge32 object) accepts and stores as such.
    ${result}=    Write Point    agent    limit    250    set-gauge-1
    Json Field Should Be    ${result}    status    successful
    Fresh Sample Value Should Be    agent    limit    250
    Agent Value Should Be    agent    ${E}.22.0    Gauge32: 250

A Write To A Read-Only Object Fails With The SNMP Error
    ${result}=    Write Point    agent    locked    1    set-ro-1
    Json Field Should Be    ${result}    status    failed
    ${reason}=    Get Json Field    ${result}    reason
    Should Contain    ${reason}    notWritable
    Good Sample Value Should Be    agent    int_negative    -42

A Value Out Of Range For The SNMP Type Is Not Written
    ${result}=    Write Point    agent    limit    -1    set-range-1
    Json Field Should Be    ${result}    status    failed
    Agent Value Should Be    agent    ${E}.22.0    Gauge32: 250

Write Batch Writes Several Points In Order
    ${result}=    Write Batch    agent    batch-1
    ...    [{"point":"setpoint","value":56},{"point":"label","value":"batch"}]
    Json Field Should Be    ${result}    status    successful
    ${results}=    Get Json Field    ${result}    results
    Length Should Be    ${results}    2
    Should Be Equal    ${results}[0][status]    successful
    Should Be Equal    ${results}[1][status]    successful
    Agent Value Should Be    agent    ${E}.20.0    INTEGER: 56
    Agent Value Should Be    agent    ${E}.21.0    STRING: "batch"

Write Batch Stops At An SNMP Error
    ${result}=    Write Batch    agent    batch-2
    ...    [{"point":"setpoint","value":57},{"point":"locked","value":1},{"point":"label","value":"never"}]
    Json Field Should Be    ${result}    status    failed
    ${results}=    Get Json Field    ${result}    results
    Length Should Be    ${results}    2
    Should Be Equal    ${results}[0][status]    successful
    Should Be Equal    ${results}[1][status]    failed
    Should Contain    ${results}[1][reason]    notWritable
    Agent Value Should Be    agent    ${E}.20.0    INTEGER: 57
    Agent Value Should Be    agent    ${E}.21.0    STRING: "batch"

# --- SNMPv3 polling ------------------------------------------------------------------------

Polls An SNMPv3 AuthNoPriv Device
    [Documentation]    SHA authentication, the password read from a file.
    ${sample}=    Good Sample    agent-v3-auth    sys_descr
    Json Field Should Be    ${sample}    value    ${SYS_DESCR}
    Good Sample    agent-v3-auth    ticks

Polls And Writes An SNMPv3 AuthPriv Device
    [Documentation]    SHA + AES-128, the privacy password read from a file.
    ${sample}=    Good Sample    agent-v3-priv    sys_descr
    Json Field Should Be    ${sample}    value    ${SYS_DESCR}
    ${result}=    Write Point    agent-v3-priv    label    "over authPriv"    set-v3-1
    Json Field Should Be    ${result}    status    successful
    Wait For Fresh Message With Field    te/device/agent-v3-priv/ot/${PROTOCOL}/sample/label
    ...    value    over authPriv    timeout=${SAMPLE_TIMEOUT}
    Agent Value Should Be    agent-v3-priv    ${E}.21.0    STRING: "over authPriv"
    Agent Should Have Received    agent-v3-priv    v3 SetRequestPDU (authPriv)

Polls An SNMPv3 SHA-256 AES-256 Device
    [Tags]    requires:snmpv3-sha2
    ${sample}=    Good Sample    agent-sha2    sys_descr
    Json Field Should Be    ${sample}    value    ${SYS_DESCR}
    Good Sample    agent-sha2    ticks

A Wrong SNMPv3 Password Degrades The Link
    [Documentation]    Every request fails USM authentication: bad samples, a degraded link, and
    ...    never a good sample.
    ${topic}=    Set Variable    te/device/agent-badauth/ot/${PROTOCOL}/status/link
    ${link}=    Wait For Fresh Message With Field    ${topic}    status    degraded    disconnected
    ...    timeout=${READY_TIMEOUT}    since=0
    ${sample}=    Wait For Fresh Message With Field    te/device/agent-badauth/ot/${PROTOCOL}/sample/sys_descr
    ...    quality    bad    timeout=${READY_TIMEOUT}    since=0
    ${samples}=    Get Messages    te/device/agent-badauth/ot/${PROTOCOL}/sample/sys_descr
    FOR    ${payload}    IN    @{samples}
        ${quality}=    Get Json Field    ${payload}    quality
        Should Be Equal    ${quality}    bad    msg=a device with a wrong password produced a good sample
    END

A Stopped Agent Gives Bad Samples And The Link Recovers
    ${mark}=    Get Message Mark
    Stop Stack Service    agent-v1
    Wait For Fresh Message With Field    te/device/agent-v1/ot/${PROTOCOL}/status/link    status    degraded    disconnected
    ...    timeout=${OUTAGE_TIMEOUT}    since=${mark}
    Wait For Fresh Message With Field    te/device/agent-v1/ot/${PROTOCOL}/sample/ticks    quality    bad
    ...    timeout=${OUTAGE_TIMEOUT}    since=${mark}
    ${mark}=    Get Message Mark
    Start Stack Service    agent-v1
    Wait For Fresh Message With Field    te/device/agent-v1/ot/${PROTOCOL}/status/link    status    connected
    ...    timeout=${RECOVERY_TIMEOUT}    since=${mark}
    Wait For Fresh Message With Field    te/device/agent-v1/ot/${PROTOCOL}/sample/ticks    quality    good
    ...    timeout=${RECOVERY_TIMEOUT}    since=${mark}
    [Teardown]    Run Keyword And Ignore Error    Start Stack Service    agent-v1

# --- notifications -------------------------------------------------------------------------

A Trap Becomes A Sample For Every Matching Point
    [Documentation]    One v2c linkDown feeds a trap point, a varbind point, and a bad sample for
    ...    a declared varbind the notification does not carry.
    ${mark}=    Get Message Mark
    Send Trap    simulator    linkdown    3
    ${sample}=    Wait For Fresh Message With Field    ${SAMPLE_PREFIX}/link_down    quality    good
    ...    timeout=${SAMPLE_TIMEOUT}    since=${mark}
    Json Field Should Be    ${sample}    value    ${LINK_DOWN}
    Json Field Should Be    ${sample}    datatype    string
    Json Field Should Be    ${sample}    addr.version    v2c
    Json Field Should Be    ${sample}    addr.pdu    trap
    Json Field Should Be    ${sample}    addr.trap    ${LINK_DOWN}

    ${index}=    Wait For Fresh Message With Field    ${SAMPLE_PREFIX}/if_index    quality    good
    ...    timeout=${SAMPLE_TIMEOUT}    since=${mark}
    ${value}=    Get Json Field    ${index}    value
    Should Be Equal As Numbers    ${value}    3
    Json Field Should Be    ${index}    addr.oid    1.3.6.1.2.1.2.2.1.1.3

    ${descr}=    Wait For Fresh Message With Field    ${SAMPLE_PREFIX}/if_descr    quality    bad
    ...    timeout=${SAMPLE_TIMEOUT}    since=${mark}
    ${error}=    Get Json Field    ${descr}    error
    Should Contain    ${error}    1.3.6.1.2.1.2.2.1.2

A Trap Point Matches Every Listed Notification
    ${mark}=    Get Message Mark
    Send Trap    simulator    linkup    4
    ${sample}=    Wait For Fresh Message With Field    ${SAMPLE_PREFIX}/link_state    value    ${LINK_UP}
    ...    timeout=${SAMPLE_TIMEOUT}    since=${mark}
    ${index}=    Wait For Fresh Message With Field    ${SAMPLE_PREFIX}/if_index    value    ${4}    ${4.0}
    ...    timeout=${SAMPLE_TIMEOUT}    since=${mark}
    Run Keyword And Expect Error    *timed out*
    ...    Wait For Fresh Message With Field    ${SAMPLE_PREFIX}/link_down    quality    good    bad
    ...    timeout=2    since=${mark}

A v1 Generic Trap Is Mapped To Its v2 Notification OID
    ${mark}=    Get Message Mark
    Send Trap    simulator    coldstart
    ${sample}=    Wait For Fresh Message With Field    ${SAMPLE_PREFIX}/cold_start    quality    good
    ...    timeout=${SAMPLE_TIMEOUT}    since=${mark}
    Json Field Should Be    ${sample}    value    ${True}
    Json Field Should Be    ${sample}    addr.version    v1
    Json Field Should Be    ${sample}    addr.trap    1.3.6.1.6.3.1.1.5.1
    Json Field Should Be    ${sample}    addr.agent_addr    10.0.0.9

Varbinds Of An Enterprise Notification Are Converted
    [Documentation]    A string, a scaled number with its unit, and an OCTET STRING in raw mode.
    ${mark}=    Get Message Mark
    Send Trap    simulator    overheat
    ${text}=    Wait For Fresh Message With Field    ${SAMPLE_PREFIX}/overheat_text    quality    good
    ...    timeout=${SAMPLE_TIMEOUT}    since=${mark}
    Json Field Should Be    ${text}    value    Pump overheated
    ${temp}=    Wait For Fresh Message With Field    ${SAMPLE_PREFIX}/overheat_temp    quality    good
    ...    timeout=${SAMPLE_TIMEOUT}    since=${mark}
    ${value}=    Get Json Field    ${temp}    value
    Should Be Equal As Numbers    ${value}    85.3
    Json Field Should Be    ${temp}    unit    °C
    ${status}=    Wait For Fresh Message With Field    ${SAMPLE_PREFIX}/overheat_status    quality    good
    ...    timeout=${SAMPLE_TIMEOUT}    since=${mark}
    Json Field Should Be    ${status}    mode    raw
    Json Field Should Be    ${status}    raw    01 02 ff

Identical Notifications Are Separate Samples
    [Documentation]    A notification is an occurrence: repeating it is not "no change".
    ${before}=    Count Messages    ${SAMPLE_PREFIX}/link_down
    Send Trap    simulator    linkdown    5
    Send Trap    simulator    linkdown    5
    Wait Until Keyword Succeeds    ${SAMPLE_TIMEOUT}s    500ms
    ...    Message Count Should Be At Least    ${SAMPLE_PREFIX}/link_down    ${before + 2}

A Trap Point Gets No Heartbeat
    [Documentation]    Every point inherits `report.max_interval = "2s"` from [connector]
    ...                (contract §5.3), but a trap point cannot be read on demand: it is
    ...                published when a notification arrives, and never as a heartbeat. The
    ...                attempt does not disturb the device's link either.
    ${mark}=    Get Message Mark
    Run Keyword And Expect Error    *timed out*
    ...    Wait For Fresh Message With Field    ${SAMPLE_PREFIX}/link_down    quality    good    bad
    ...    timeout=7    since=${mark}
    ${link}=    Wait For Retained    ${LINK_TOPIC}    timeout=${SAMPLE_TIMEOUT}
    ${status}=    Get Json Field    ${link}    status
    Should Be Equal    ${status}    connected

An Inform Is Acknowledged And Delivered
    [Documentation]    snmpinform exits non-zero unless it receives the Response.
    ${mark}=    Get Message Mark
    Send Trap    simulator    inform    hello
    ${sample}=    Wait For Fresh Message With Field    ${SAMPLE_PREFIX}/inform_text    value    hello
    ...    timeout=${SAMPLE_TIMEOUT}    since=${mark}
    Json Field Should Be    ${sample}    addr.pdu    inform

A Community The Device Does Not Accept Is Dropped
    ${mark}=    Get Message Mark
    Send Trap    simulator    linkdown    6    community=private
    ${rc}=    Send Trap    simulator    inform    rejected    community=private    expect_rc=any
    Should Not Be Equal As Integers    ${rc}    0    msg=an inform with a rejected community was acknowledged
    Run Keyword And Expect Error    *timed out*
    ...    Wait For Fresh Message With Field    ${SAMPLE_PREFIX}/link_down    quality    good    bad
    ...    timeout=3    since=${mark}
    Run Keyword And Expect Error    *timed out*
    ...    Wait For Fresh Message With Field    ${SAMPLE_PREFIX}/inform_text    quality    good    bad
    ...    timeout=1    since=${mark}

A Sender No Device Is Configured For Is Ignored
    ${mark}=    Get Message Mark
    Send Trap    stranger    linkdown    7
    ${rc}=    Send Trap    stranger    inform    stranger    expect_rc=any
    Should Not Be Equal As Integers    ${rc}    0    msg=an inform from an unconfigured sender was acknowledged
    Run Keyword And Expect Error    *timed out*
    ...    Wait For Fresh Message With Field    ${SAMPLE_PREFIX}/link_down    quality    good    bad
    ...    timeout=3    since=${mark}
    # ...and the receiver still works for the configured device afterwards.
    Send Trap    simulator    linkdown    8
    Wait For Fresh Message With Field    ${SAMPLE_PREFIX}/link_down    quality    good
    ...    timeout=${SAMPLE_TIMEOUT}    since=${mark}

A v3 AuthPriv Trap Is Accepted
    [Documentation]    SHA + AES-128 from the engine ID configured for the device.
    ${mark}=    Get Message Mark
    Send Trap    simulator    v3-linkdown    21
    ${sample}=    Wait For Fresh Message With Field    ${SAMPLE_PREFIX}/link_down    quality    good
    ...    timeout=${SAMPLE_TIMEOUT}    since=${mark}
    Json Field Should Be    ${sample}    addr.version    v3
    Json Field Should Be    ${sample}    addr.pdu    trap
    Wait For Fresh Message With Field    ${SAMPLE_PREFIX}/if_index    value    ${21}    ${21.0}
    ...    timeout=${SAMPLE_TIMEOUT}    since=${mark}

A v3 Trap With A Wrong Key Is Dropped
    ${mark}=    Get Message Mark
    Send Trap    simulator    v3-linkdown    22    env=-e V3_PRIV_PASS=wrong-password-9
    Send Trap    simulator    v3-linkdown    23    env=-e V3_AUTH_PASS=wrong-password-9
    Run Keyword And Expect Error    *timed out*
    ...    Wait For Fresh Message With Field    ${SAMPLE_PREFIX}/if_index    value    ${22}    ${22.0}    ${23}    ${23.0}
    ...    timeout=3    since=${mark}
    # ...and a correctly keyed one still gets through afterwards.
    Send Trap    simulator    v3-linkdown    24
    Wait For Fresh Message With Field    ${SAMPLE_PREFIX}/if_index    value    ${24}    ${24.0}
    ...    timeout=${SAMPLE_TIMEOUT}    since=${mark}

An Unauthenticated v3 Trap Is Dropped And The Device Keeps Working
    [Documentation]    The device is configured authPriv, so a noAuthNoPriv trap carrying its
    ...    user name -- which travels in the clear in every v3 message -- must be dropped, and
    ...    the device must keep accepting properly authenticated traps afterwards.
    ...
    ...    What this case does NOT establish: every trap device in connector.toml pins
    ...    `engine_id`, and the simulator sends this trap from that same engine, so the
    ...    receiver's engine-LEARNING branch is never reached and the case passes with or
    ...    without the fix that guards it. Discriminating that needs a device whose engine is
    ...    unpinned when the case runs (see TODO.md); the property is covered at unit level by
    ...    `an_unauthenticated_notification_teaches_an_authenticated_user_nothing`, which does
    ...    fail without the fix.
    ${mark}=    Get Message Mark
    Send Trap    simulator    v3-linkdown-noauth    31
    Run Keyword And Expect Error    *timed out*
    ...    Wait For Fresh Message With Field    ${SAMPLE_PREFIX}/if_index    value    ${31}    ${31.0}
    ...    timeout=3    since=${mark}
    # ...and the device still accepts a properly authenticated trap afterwards, which it cannot
    # do if the unauthenticated one replaced its engine ID or re-derived its keys.
    Send Trap    simulator    v3-linkdown    32
    Wait For Fresh Message With Field    ${SAMPLE_PREFIX}/if_index    value    ${32}    ${32.0}
    ...    timeout=${SAMPLE_TIMEOUT}    since=${mark}

A v3 Inform Is Acknowledged And Delivered
    [Documentation]    snmpinform first discovers the receiver's engine ID, then sends the inform
    ...    authPriv; it exits non-zero unless the Response arrives.
    ${mark}=    Get Message Mark
    Send Trap    simulator    v3-inform    hello-v3
    ${sample}=    Wait For Fresh Message With Field    ${SAMPLE_PREFIX}/inform_text    value    hello-v3
    ...    timeout=${SAMPLE_TIMEOUT}    since=${mark}
    Json Field Should Be    ${sample}    addr.version    v3
    Json Field Should Be    ${sample}    addr.pdu    inform

A v3 Inform With A Wrong Key Is Not Acknowledged
    ${mark}=    Get Message Mark
    ${rc}=    Send Trap    simulator    v3-inform    wrong-key    env=-e V3_AUTH_PASS=wrong-password-9    expect_rc=any
    Should Not Be Equal As Integers    ${rc}    0    msg=an inform with a wrong key was acknowledged
    Run Keyword And Expect Error    *timed out*
    ...    Wait For Fresh Message With Field    ${SAMPLE_PREFIX}/inform_text    value    wrong-key
    ...    timeout=2    since=${mark}

A Trusted Forwarder's Notification Is Routed By snmpTrapAddress
    [Documentation]    `forwarded` sends a linkDown carrying snmpTrapAddress.0 = its own address to
    ...    snmptrapd in `forwarder`, which passes it on to the connector. The datagram comes from
    ...    the forwarder, but the sample belongs to `branch-switch` (host `forwarded`).
    ${forwarder_ip}=    Service Address    forwarder
    ${sender_ip}=    Service Address    forwarded
    ${topic}=    Set Variable    te/device/branch-switch/ot/${PROTOCOL}/sample/link_down
    ${mark}=    Get Message Mark
    Send Trap    forwarded    linkdown-addressed    31
    ${sample}=    Wait For Fresh Message With Field    ${topic}    quality    good
    ...    timeout=${SAMPLE_TIMEOUT}    since=${mark}
    Json Field Should Be    ${sample}    value    ${LINK_DOWN}
    Json Field Should Be    ${sample}    addr.forwarder    ${forwarder_ip}
    Json Field Should Be    ${sample}    addr.source    ${sender_ip}
    Wait For Fresh Message With Field    te/device/branch-switch/ot/${PROTOCOL}/sample/if_index    value    ${31}    ${31.0}
    ...    timeout=${SAMPLE_TIMEOUT}    since=${mark}
    Run Keyword And Expect Error    *timed out*
    ...    Wait For Fresh Message With Field    ${SAMPLE_PREFIX}/if_index    value    ${31}    ${31.0}
    ...    timeout=1    since=${mark}

A Forwarder's Notification Without snmpTrapAddress Is Dropped
    [Documentation]    A plain v2c linkDown through the same forwarder: net-snmp's snmptrapd adds
    ...    no snmpTrapAddress.0 of its own, so nothing says whose notification it is and no device
    ...    gets it.
    ${mark}=    Get Message Mark
    Send Trap    forwarded    linkdown    32
    Run Keyword And Expect Error    *timed out*
    ...    Wait For Fresh Message With Field    te/device/branch-switch/ot/${PROTOCOL}/sample/if_index    value    ${32}    ${32.0}
    ...    timeout=3    since=${mark}
    Run Keyword And Expect Error    *timed out*
    ...    Wait For Fresh Message With Field    ${SAMPLE_PREFIX}/if_index    value    ${32}    ${32.0}
    ...    timeout=1    since=${mark}

No Secret Appears In Anything The Connector Publishes Or Logs
    [Documentation]    After polling, writes and v3 notifications have all happened: no community-
    ...    independent secret (v3 passwords, including those read from files) in any message the
    ...    suite saw — retained status, capabilities, samples, command results — nor in the log.
    Good Sample    agent-v3-priv    sys_descr
    ${lib}=    Get Library Instance    MqttClient
    ${everything}=    Evaluate    "\\n".join(p for entries in $lib._messages.values() for _t, p in entries)
    Should Not Be Empty    ${everything}
    ${project}=    DeviceLibrary.Get Compose Project Name
    ${rc}    ${logs}=    Run And Return Rc And Output    docker compose -p ${project} logs --no-color connector
    Should Be Equal As Integers    ${rc}    0
    FOR    ${secret}    IN    @{SECRETS}
        Should Not Contain    ${everything}    ${secret}    msg=a published message contains the secret ${secret}
        Should Not Contain    ${logs}    ${secret}    msg=the connector log contains the secret ${secret}
    END

# --- flows ---------------------------------------------------------------------------------

Every Notification Raises An Event
    [Documentation]    ot-event with `every = true`: identical traps are separate events.
    [Tags]    flows
    # The flows runner starts independently of the connector: wait until it reacts at all.
    Wait Until Keyword Succeeds    90s    3s    Trap Raises Event
    ${before}=    Count Messages    ${EVENT_TOPIC}
    Send Trap    simulator    linkdown    9
    Send Trap    simulator    linkdown    9
    Wait Until Keyword Succeeds    ${SAMPLE_TIMEOUT}s    500ms
    ...    Message Count Should Be At Least    ${EVENT_TOPIC}    ${before + 2}
    ${event}=    Get Message    ${EVENT_TOPIC}
    Json Field Should Be    ${event}    text    Link down on switch

LinkDown Raises An Alarm That LinkUp Clears
    [Tags]    flows
    Wait Until Keyword Succeeds    90s    3s    Trap Raises Event
    Send Trap    simulator    linkdown    10
    Wait Until Keyword Succeeds    ${SAMPLE_TIMEOUT}s    500ms    Latest Message Should Contain    ${ALARM_TOPIC}    Link down
    Send Trap    simulator    linkup    10
    Wait Until Keyword Succeeds    ${SAMPLE_TIMEOUT}s    500ms    Latest Message Should Be Empty    ${ALARM_TOPIC}


Measurement Output Batches A Polled Pass Into One Measurement
    [Documentation]    In measurement output the connector publishes the thin-edge measurement
    ...                itself: the device's polled points of one group share the read time of
    ...                their call, so they arrive in one message (contract §5.4, §5.5). A string
    ...                becomes no series; a boolean becomes 1.
    ${payload}=    Wait For Message Containing    ${MEASURE_TOPIC}    "gauge_float"    timeout=${READY_TIMEOUT}
    ${gauge}=    Get Json Field    ${payload}    snmp.gauge_float
    Should Be Equal As Numbers    ${gauge}    42
    ${flag}=    Get Json Field    ${payload}    snmp.gauge_bool
    Should Be Equal As Numbers    ${flag}    1
    Should Contain    ${payload}    "int_negative"
    Should Not Contain    ${payload}    sys_descr

Measurement Output Names Series As The Measurement Flow Does
    [Documentation]    A point's meta.measurement names its group and series.
    ${payload}=    Wait For Message Containing    te/device/snmpm///m/Agent    "Seed"    timeout=${READY_TIMEOUT}
    ${seed}=    Get Json Field    ${payload}    Agent.Seed
    Should Be Equal As Numbers    ${seed}    1234

Measurement Output Publishes The Units Metadata
    [Documentation]    The connector publishes the units of a measurement topic, retained, on
    ...                <topic>/meta: only the series with a unit.
    # Read from the broker: a retained message is delivered once per subscription.
    Wait For Message Containing    ${MEASURE_TOPIC}    "time"    timeout=${READY_TIMEOUT}
    ${payload}=    Fetch Retained    ${MEASURE_TOPIC}/meta    timeout=${SAMPLE_TIMEOUT}
    # The keys hold a dot ("<group>.<series>"), so they are matched as text.
    Should Contain    ${payload}    "snmp.int_negative":{"unit":"u"}
    Should Not Contain    ${payload}    gauge_float

Measurement Output Publishes Samples Only For Points Flows Read
    [Documentation]    The alarm point still publishes samples, marked "measured" so
    ...                ot-measurement does not convert them again; a plain point publishes none.
    ${payload}=    Wait For Sample    ${MEASURE_SAMPLE_PREFIX}/gauge_float    timeout=${READY_TIMEOUT}
    ${measured}=    Get Json Field    ${payload}    measured
    Should Be Equal    ${measured}    ${True}
    No New Messages On Topic    ${MEASURE_SAMPLE_PREFIX}/int_negative    timeout=5

Alarm Point Raises Its Alarm In Measurement Output
    [Documentation]    ot-alarm raises the alarm declared on the point from its measured sample.
    Wait Until Keyword Succeeds    ${MEASURE_FLOWS_TIMEOUT}s    2s    Fetch Retained    te/device/snmpm///a/snmpm_gauge_high    timeout=2
    ${payload}=    Fetch Retained    te/device/snmpm///a/snmpm_gauge_high    timeout=2
    ${severity}=    Get Json Field    ${payload}    severity
    Should Be Equal    ${severity}    major


*** Keywords ***
Setup SNMP Stack
    Setup OT Stack    snmp
    # Topics are plain strings: te/device/<device>/ot/${PROTOCOL}/sample/<point>.
    Wait For Fresh Message With Field    ${LINK_TOPIC}    status    connected
    ...    timeout=${READY_TIMEOUT}    since=0
    Wait For Fresh Message With Field    te/device/agent/ot/${PROTOCOL}/status/link    status    connected
    ...    timeout=${READY_TIMEOUT}    since=0

Send Trap
    [Documentation]    Run send-trap in a notification container. `community` sets COMMUNITY;
    ...    `env` passes further `-e NAME=value` options to `docker compose exec` (a wrong v3 key);
    ...    `expect_rc=any` returns the exit code instead of requiring 0.
    [Arguments]    ${service}    @{args}    ${community}=public    ${expect_rc}=0    ${env}=${EMPTY}
    ${project}=    DeviceLibrary.Get Compose Project Name
    ${argline}=    Catenate    @{args}
    # `timeout` bounds the notification: snmpinform's own -r/-t bound each request, but a
    # receiver that keeps answering a confirmed message with a Report leaves net-snmp
    # re-synchronising and re-sending forever. Without this the whole suite wedges on one
    # notification instead of failing the test that sent it; 124 is timeout's own exit code.
    #
    # -k matters: plain `timeout` sends only SIGTERM, and `docker compose exec` ignores it
    # while attached -- an orphaned exec here survived its SIGTERM by 17 minutes. Without the
    # escalation to SIGKILL the bound would fire and change nothing.
    ${rc}    ${output}=    Run And Return Rc And Output
    ...    timeout -k 5 30 docker compose -p ${project} exec -T -e COMMUNITY=${community} ${env} ${service} send-trap ${argline}
    IF    ${rc} == 124
        Fail    send-trap ${argline} from ${service} did not return within 30s: ${output}
    END
    Log    send-trap ${argline} (${community} ${env}) from ${service}: rc=${rc} ${output}
    IF    "${expect_rc}" != "any"
        Should Be Equal As Integers    ${rc}    ${expect_rc}    msg=send-trap failed: ${output}
    END
    RETURN    ${rc}

Service Address
    [Documentation]    The IPv4 address a stack service has on the compose network.
    [Arguments]    ${service}
    ${project}=    DeviceLibrary.Get Compose Project Name
    ${rc}    ${output}=    Run And Return Rc And Output
    ...    docker compose -p ${project} exec -T simulator getent ahostsv4 ${service}
    Should Be Equal As Integers    ${rc}    0    msg=cannot resolve ${service}: ${output}
    ${address}=    Fetch From Left    ${output}    ${SPACE}
    RETURN    ${address.strip()}

Agent Value Should Be
    [Documentation]    Read an OID straight from a polled agent with net-snmp (from the
    ...    `simulator` container), independently of the connector. `expected` is net-snmp's
    ...    "TYPE: value" rendering. Each agent is asked with the credentials it accepts.
    [Arguments]    ${service}    ${oid}    ${expected}
    ${project}=    DeviceLibrary.Get Compose Project Name
    ${creds}=    Set Variable If    "${service}" == "agent-v3-priv"
    ...    -v3 -l authPriv -u rw-priv -a SHA -A rw-auth-pass-1 -x AES -X rw-priv-pass-1
    ...    -v2c -c public
    ${rc}    ${output}=    Run And Return Rc And Output
    ...    docker compose -p ${project} exec -T simulator snmpget -m '' -On ${creds} ${service} ${oid}
    Should Be Equal As Integers    ${rc}    0    msg=snmpget ${service} ${oid} failed: ${output}
    Should Contain    ${output}    = ${expected}

Agent Should Have Received
    [Documentation]    The agent's request log (agent.py logs one line per request) has a request
    ...    matching `text` from anyone but itself (its healthcheck asks from 127.0.0.1).
    [Arguments]    ${service}    ${text}
    ${requests}=    Agent Requests    ${service}
    Should Contain    ${requests}    ${text}    msg=${service} received no "${text}" request

Agent Should Not Have Received
    [Arguments]    ${service}    ${text}
    ${requests}=    Agent Requests    ${service}
    Should Not Contain    ${requests}    ${text}    msg=${service} received a "${text}" request

Agent Requests
    [Arguments]    ${service}
    ${project}=    DeviceLibrary.Get Compose Project Name
    ${rc}    ${output}=    Run And Return Rc And Output
    ...    docker compose -p ${project} logs --no-color ${service} | grep 'request from' | grep -v 'request from 127.0.0.1'
    RETURN    ${output}

Write Point
    [Documentation]    Publish a `write` command (value as JSON text) and return its final result.
    [Arguments]    ${device}    ${point}    ${value}    ${id}
    ${topic}=    Set Variable    te/device/${device}/ot/${PROTOCOL}/cmd/write/${id}
    Publish Message    ${topic}    {"status":"init","point":"${point}","value":${value}}    retain=True
    ${result}=    Wait For Fresh Message With Field    ${topic}    status    successful    failed
    ...    timeout=${SAMPLE_TIMEOUT}    since=0
    RETURN    ${result}

Write Batch
    [Arguments]    ${device}    ${id}    ${writes}
    ${topic}=    Set Variable    te/device/${device}/ot/${PROTOCOL}/cmd/write-batch/${id}
    Publish Message    ${topic}    {"status":"init","writes":${writes}}    retain=True
    ${result}=    Wait For Fresh Message With Field    ${topic}    status    successful    failed
    ...    timeout=${SAMPLE_TIMEOUT}    since=0
    RETURN    ${result}

Link Info Should Be
    [Arguments]    ${device}    ${host}    ${port}    ${version}
    ${payload}=    Wait For Retained    te/device/${device}/ot/${PROTOCOL}/status/link    timeout=${READY_TIMEOUT}
    ${info}=    Get Json Field    ${payload}    info
    ${expected}=    Create Dictionary    host=${host}    port=${{ int($port) }}    version=${version}
    Dictionaries Should Be Equal    ${info}    ${expected}

Good Sample
    [Documentation]    The next fresh sample of a point; it must be good.
    [Arguments]    ${device}    ${point}
    ${sample}=    Wait For Fresh Message With Field    te/device/${device}/ot/${PROTOCOL}/sample/${point}    quality    good    bad
    ...    timeout=${SAMPLE_TIMEOUT}
    Json Field Should Be    ${sample}    quality    good
    RETURN    ${sample}

Bad Sample
    [Arguments]    ${device}    ${point}
    ${sample}=    Wait For Fresh Message With Field    te/device/${device}/ot/${PROTOCOL}/sample/${point}    quality    good    bad
    ...    timeout=${SAMPLE_TIMEOUT}
    Json Field Should Be    ${sample}    quality    bad
    RETURN    ${sample}

Good Sample Value
    [Arguments]    ${device}    ${point}
    ${sample}=    Good Sample    ${device}    ${point}
    ${value}=    Get Json Field    ${sample}    value
    RETURN    ${value}

Good Sample Value Should Be
    [Arguments]    ${device}    ${point}    ${expected}
    ${value}=    Good Sample Value    ${device}    ${point}
    Should Be Equal As Numbers    ${value}    ${expected}

Fresh Sample Value Should Be
    [Documentation]    Wait for a sample carrying the value (the write may land a poll later).
    [Arguments]    ${device}    ${point}    ${expected}
    Wait For Fresh Message With Field    te/device/${device}/ot/${PROTOCOL}/sample/${point}    value
    ...    ${{ int($expected) }}    ${{ float($expected) }}    timeout=${SAMPLE_TIMEOUT}    since=0

Trap Raises Event
    ${before}=    Count Messages    ${EVENT_TOPIC}
    Send Trap    simulator    linkdown    1
    Wait Until Keyword Succeeds    3s    250ms    Message Count Should Be At Least    ${EVENT_TOPIC}    ${before + 1}

Json Field Should Be
    [Arguments]    ${payload}    ${field}    ${expected}
    ${value}=    Get Json Field    ${payload}    ${field}
    Should Be Equal    ${value}    ${expected}

Count Messages
    [Arguments]    ${topic}
    ${messages}=    Get Messages    ${topic}
    ${count}=    Get Length    ${messages}
    RETURN    ${count}

Message Count Should Be At Least
    [Arguments]    ${topic}    ${minimum}
    ${count}=    Count Messages    ${topic}
    Should Be True    ${count} >= ${minimum}    msg=${count} messages on ${topic}, expected at least ${minimum}

Latest Message Should Contain
    [Arguments]    ${topic}    ${text}
    ${payload}=    Get Message    ${topic}
    Should Contain    ${payload}    ${text}

Latest Message Should Be Empty
    [Arguments]    ${topic}
    ${payload}=    Get Message    ${topic}
    Should Be Empty    ${payload}
