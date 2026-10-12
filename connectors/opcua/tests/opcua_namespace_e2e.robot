*** Settings ***
Documentation       End-to-end tests for OPC UA namespace tables (openspec change
...                 opcua-namespace-table): node ids written against one NamespaceArray order
...                 keep reading the right nodes on servers that number their namespaces
...                 differently. The stack (docker-compose.namespaces.yaml) runs two simulators,
...                 ns-sim-a with urn:tedge:opcua-sim at index 2 -- the order the tables record --
...                 and ns-sim-b with it at index 3. The configuration is
...                 namespaces/connector.toml with the point library namespaces/points.d.
...
...                 Run via:  just test-e2e opcua   /   just test-e2e-c opcua

Resource            ../../_shared/stack.resource
Library             Collections
Library             OperatingSystem

Suite Setup         Setup OT Stack    opcua    compose_file=${CURDIR}/../docker-compose.namespaces.yaml
Suite Teardown      Teardown OT Stack


*** Variables ***
${PROTOCOL}             opcua
${READY_TIMEOUT}        90
${SAMPLE_TIMEOUT}       15
${RECOVERY_TIMEOUT}     90
# Untranslated subscribed points are tried again every 30 s (RETRY_INTERVAL); polled ones at
# their next poll.
${PUSH_RETRY_TIMEOUT}   75
${SIM_URI}              urn:tedge:opcua-sim


*** Test Cases ***
Library Points Read Through The Library's Table
    [Documentation]    The library's ids say ns=2. ns-sim-a lists the namespace at 2, ns-sim-b at
    ...                3: both read the simulator's Temperature, and the sample shows the address
    ...                as configured.
    FOR    ${device}    IN    ns-a    ns-b
        ${payload}=    Point Should Be    ${device}    temperature    21.5
        ${node}=    Get Json Field    ${payload}    addr.node_id
        Should Be Equal    ${node}    ns=2;s=Temperature
    END

The Moved Namespace Is Logged Once
    [Documentation]    A namespace the server lists at another index than the table is logged at
    ...                info, once per session, in the same words by both builds.
    Wait Until Keyword Succeeds    ${READY_TIMEOUT}s    2s
    ...    Connector Log Should Contain    ns-b: namespace ${SIM_URI} local 2 -> server 3
    ${count}=    Connector Log Count    ns-b: namespace ${SIM_URI} local 2 -> server 3
    Should Be Equal As Integers    ${count}    1

A Pushed Point Uses The Translation
    [Documentation]    `ticks` is subscribed: its monitored item is created on the translated node,
    ...                so its value keeps changing on ns-b.
    ${first}=    Point Should Be Good    ns-b    ticks
    ${second}=    Point Should Be Good    ns-b    ticks
    ${a}=    Get Json Field    ${first}    value
    ${b}=    Get Json Field    ${second}    value
    Should Be True    ${b} > ${a}

A Write Goes To The Translated Node
    [Documentation]    `setpoint` is ns=2;s=Setpoint in the library and sits at index 3 on ns-sim-b.
    ${topic}=    Set Variable    te/device/ns-b/ot/${PROTOCOL}/cmd/write/ns-setpoint-1
    Publish Message    ${topic}    {"status":"init","point":"setpoint","value":42}    retain=True
    Wait For Message Containing    ${topic}    "status":"successful"    timeout=${SAMPLE_TIMEOUT}
    Wait Until Keyword Succeeds    ${SAMPLE_TIMEOUT}s    1s    Point Should Be    ns-b    setpoint    42

A Namespace The Server Does Not List Fails Only Its Point
    [Documentation]    `ghost` names urn:not-on-the-server through the device's table. It reports
    ...                why; the device's other points keep publishing and its link stays up.
    Point Should Fail With    ns-a    ghost    namespace not found: urn:not-on-the-server
    Point Should Be    ns-a    temperature    21.5
    ${link}=    Wait For Retained    te/device/ns-a/ot/${PROTOCOL}/status/link    timeout=${READY_TIMEOUT}
    ${status}=    Get Json Field    ${link}    status
    Should Be Equal    ${status}    connected

A Namespace Added While Connected Is Picked Up
    [Documentation]    OPC UA lets a server add namespaces during a session (10000-5 §6.3.1).
    ...                urn:tedge:opcua-sim:late does not exist yet, so its points report why; once
    ...                the simulator adds it, the polled point reads at its next poll and the
    ...                pushed point gets its monitored item at the next retry. (The C build renews
    ...                its session for it: open62541 maps namespace indexes per session.)
    Point Should Fail With    ns-a    late_polled    namespace not found: urn:tedge:opcua-sim:late
    DeviceLibrary.Execute Command    cmd=touch /control/add-late
    Wait Until Keyword Succeeds    ${SAMPLE_TIMEOUT}s    1s    Point Should Be    ns-a    late_polled    7.5
    Wait For Message Containing    te/device/ns-a/ot/${PROTOCOL}/sample/late_pushed    "quality":"good"
    ...    timeout=${PUSH_RETRY_TIMEOUT}
    ${payload}=    Get Message    te/device/ns-a/ot/${PROTOCOL}/sample/late_pushed
    ${value}=    Get Json Field    ${payload}    value
    Should Be Equal As Numbers    ${value}    7.5

Ids Keep Their Node When The Server Renumbers Across A Restart
    [Documentation]    ns-sim-a restarts with an extra namespace first, moving its nodes from
    ...                ns=2 to ns=3. The points with a table follow; the device without one
    ...                (ns-plain) now asks for a node that does not exist -- the silent failure the
    ...                table prevents.
    DeviceLibrary.Execute Command    cmd=touch /control/shift
    ${mark}=    Get Message Mark
    Restart Stack Service    ns-sim-a
    Wait Until Keyword Succeeds    ${RECOVERY_TIMEOUT}s    2s
    ...    Connector Log Should Contain    ns-a: namespace ${SIM_URI} local 2 -> server 3
    Wait Until Keyword Succeeds    ${RECOVERY_TIMEOUT}s    1s    Point Should Be    ns-a    temperature    21.5
    Wait Until Keyword Succeeds    ${RECOVERY_TIMEOUT}s    1s
    ...    Point Should Fail With    ns-plain    temperature    BadNodeIdUnknown
    Point Should Fail With    ns-a    ghost    namespace not found: urn:not-on-the-server


*** Keywords ***
Point Should Be Good
    [Documentation]    The next sample of a point is good; returns it.
    [Arguments]    ${device}    ${point}
    ${payload}=    Wait For Sample    te/device/${device}/ot/${PROTOCOL}/sample/${point}
    ...    timeout=${READY_TIMEOUT}
    ${quality}=    Get Json Field    ${payload}    quality
    Should Be Equal    ${quality}    good    ${device}/${point}: ${payload}
    RETURN    ${payload}

Point Should Be
    [Documentation]    The next sample of a point is good, with this value; returns it.
    [Arguments]    ${device}    ${point}    ${expected}
    ${payload}=    Point Should Be Good    ${device}    ${point}
    ${value}=    Get Json Field    ${payload}    value
    Should Be Equal As Numbers    ${value}    ${expected}    ${device}/${point}
    RETURN    ${payload}

Point Should Fail With
    [Documentation]    The next sample of a point is bad, and its error contains `text`.
    [Arguments]    ${device}    ${point}    ${text}
    ${payload}=    Wait For Sample    te/device/${device}/ot/${PROTOCOL}/sample/${point}
    ...    timeout=${READY_TIMEOUT}
    ${quality}=    Get Json Field    ${payload}    quality
    Should Be Equal    ${quality}    bad    ${device}/${point}: ${payload}
    ${error}=    Get Json Field    ${payload}    error
    Should Contain    ${error}    ${text}

Connector Logs
    ${project}=    DeviceLibrary.Get Compose Project Name
    ${rc}    ${output}=    Run And Return Rc And Output
    ...    docker compose -p ${project} logs --no-color connector
    Should Be Equal As Integers    ${rc}    0
    RETURN    ${output}

Connector Log Should Contain
    [Arguments]    ${text}
    ${logs}=    Connector Logs
    Should Contain    ${logs}    ${text}

Connector Log Count
    [Arguments]    ${text}
    ${logs}=    Connector Logs
    ${count}=    Evaluate    $logs.count($text)
    RETURN    ${count}
