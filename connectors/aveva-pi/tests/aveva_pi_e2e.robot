*** Settings ***
Documentation       End-to-end tests for the tedge-dot AVEVA PI Web API connector: batched snapshot
...                 reads, Channels push, recorded polling, outage and restart backfill, writes, and
...                 every authentication method, against our PI Web API simulator
...                 (connectors/aveva-pi/sim; seeds in sim/seed.json). Each scenario's simulator is
...                 its own fault domain (docker-compose.yaml); the suite drives it through the
...                 simulator's `/_sim` control API from inside its container.
...
...                 Run via:  just test-e2e aveva-pi   /   just test-e2e-c aveva-pi

Resource            ../../_shared/stack.resource
Library             Collections
Library             String

Suite Setup         Setup PI Stack
Suite Teardown      Teardown OT Stack


*** Variables ***
${PROTOCOL}             aveva-pi
${SERVICE}              tedge-dot
${CAPS_TOPIC}           te/device/main/service/${SERVICE}/ot/capabilities
${HEALTH_TOPIC}         te/device/main/service/${SERVICE}/status/health
${STATE_DIR}            /var/lib/tedge-dot/aveva-pi/${SERVICE}

${READY_TIMEOUT}        90
${SAMPLE_TIMEOUT}       10
# request_timeout 2s, then the runtime's reconnect backoff.
${OUTAGE_TIMEOUT}       30
${RECOVERY_TIMEOUT}     60
# Measurement output (connector-measure.toml): the `pim` device on its own simulator.
${MEASURE_TOPIC}        te/device/pim///m/${PROTOCOL}


*** Test Cases ***
Connector Publishes Capability Descriptor
    ${payload}=    Wait For Retained    ${CAPS_TOPIC}    timeout=${READY_TIMEOUT}
    ${protocol}=    Get Json Field    ${payload}    protocol
    Should Be Equal    ${protocol}    aveva-pi
    ${subscribe}=    Get Json Field    ${payload}    subscribe
    Should Be True    ${subscribe}
    ${features}=    Get Json Field    ${payload}    features
    FOR    ${feature}    IN    polling    subscribe    bulk_read    backfill
        List Should Contain Value    ${features}    ${feature}
    END
    ${kinds}=    Get Json Field    ${payload}    point_kinds
    FOR    ${kind}    IN    point    attribute    stream
        List Should Contain Value    ${kinds}    ${kind}
    END
    ${verbs}=    Get Json Field    ${payload}    command_verbs
    List Should Contain Value    ${verbs}    write
    List Should Contain Value    ${verbs}    write-batch

Service Health Is Up
    ${payload}=    Wait For Retained    ${HEALTH_TOPIC}    timeout=${READY_TIMEOUT}
    ${status}=    Get Json Field    ${payload}    status
    Should Be Equal    ${status}    up

Link Status Carries The Endpoint
    ${payload}=    Wait For Retained    te/device/pi/ot/${PROTOCOL}/status/link    timeout=${READY_TIMEOUT}
    ${info}=    Get Json Field    ${payload}    info
    ${expected}=    Create Dictionary    base_url=https://simulator:8443/piwebapi    delivery=channel
    Dictionaries Should Be Equal    ${info}    ${expected}

Snapshot Poll Is One Request Per Device
    [Documentation]    WebIds are resolved once on connect; each poll of the device's points is
    ...    one streamsets/value request (no lookups per poll).
    Wait For Sample Of    pi-snap    sinusoid
    ${before}=    Sim Counters    sim-snapshot
    Sleep    6.5s
    ${after}=    Sim Counters    sim-snapshot
    ${polls}=    Evaluate    $after.get("streamsets/value", 0) - $before.get("streamsets/value", 0)
    Should Be True    3 <= ${polls} <= 4    msg=${polls} streamsets/value requests in 6.5 s at a 2 s poll
    # Only the one tag the server lacks is looked up again, every resolve_retry (5 s).
    ${lookups}=    Evaluate    $after.get("points", 0) - $before.get("points", 0)
    Should Be True    ${lookups} <= 2    msg=${lookups} point lookups in 6.5 s: resolved points were looked up again

Snapshot Sample Carries Read Time And PI Timestamp
    ${s}=    Wait For Sample Of    pi-snap    count16
    ${value}=    Get Json Field    ${s}    value
    Should Be Equal As Numbers    ${value}    -42
    ${addr}=    Get Json Field    ${s}    addr
    Should Start With    ${addr}[web_id]    F1DP
    Should Be Equal    ${addr}[point_type]    Int16
    ${ts}=    Get Json Field    ${s}    ts
    Should Not Be Equal    ${ts}    ${addr}[timestamp]    msg=a polled sample carries the read time

Digital State As Name And Code
    ${name}=    Wait For Sample Of    pi-snap    pump_name
    ${code}=    Wait For Sample Of    pi-snap    pump_code
    ${n}=    Get Json Field    ${name}    value
    ${c}=    Get Json Field    ${code}    value
    Should Be True    "${n}" in ("Running", "Stopped")
    IF    "${n}" == "Running"
        Should Be Equal As Numbers    ${c}    1
    ELSE
        Should Be Equal As Numbers    ${c}    0
    END

System State Is A Bad Sample
    ${s}=    Wait For Sample Of    pi-snap    io
    Should Be Equal    ${{ json.loads($s)["quality"] }}    bad
    Should Be Equal    ${{ json.loads($s)["error"] }}    I/O Timeout

Questionable Value Stays Good With A Flag
    ${s}=    Wait For Sample Of    pi-snap    questionable
    Should Be Equal    ${{ json.loads($s)["quality"] }}    good
    Should Be Equal    ${{ json.loads($s)["addr"]["flags"] }}    ${{ ["questionable"] }}

Unknown Tag Is Not Found Without Taking The Link Down
    ${s}=    Wait For Sample Of    pi-snap    missing
    Should Be Equal    ${{ json.loads($s)["quality"] }}    bad
    Should Start With    ${{ json.loads($s)["error"] }}    not found:
    Link Should Be    pi-snap    connected

Units Come From PI Unless Configured
    ${s}=    Wait For Sample Of    pi-snap    sinusoid
    Should Be Equal    ${{ json.loads($s)["unit"] }}    degC
    ${s}=    Wait For Sample Of    pi-snap    pressure
    Should Be Equal    ${{ json.loads($s)["unit"] }}    bar    msg=the AF attribute's own unit
    ${s}=    Wait For Sample Of    pi-snap    units_override
    Should Be Equal    ${{ json.loads($s)["unit"] }}    K

Raw Mode Carries The PI Value As JSON Text
    ${s}=    Wait For Sample Of    pi-snap    sinusoid_raw
    ${raw}=    Get Json Field    ${s}    raw
    ${text}=    Evaluate    bytes.fromhex($raw.replace(" ", "")).decode()
    Evaluate    float($text)

Numeric Write
    ${r}=    Write Point    pi-snap    setpoint    42.5    w-num
    Should Be Equal    ${{ json.loads($r)["status"] }}    successful
    ${writes}=    Sim Get    sim-snapshot    writes
    Should Be Equal    ${writes}[SETPOINT][body]    ${{ {"Value": 42.5} }}
    Should Be Equal    ${writes}[SETPOINT][x_requested_with]    XMLHttpRequest
    Wait Until Keyword Succeeds    10s    1s    Next Sample Value Should Be    pi-snap    setpoint    42.5

Digital Write By Name
    ${r}=    Write Point    pi-snap    pump_name    "Stopped"    w-dig
    Should Be Equal    ${{ json.loads($r)["status"] }}    successful
    ${writes}=    Sim Get    sim-snapshot    writes
    Should Be Equal    ${writes}[PUMP01.STATE][body]
    ...    ${{ {"Value": {"Name": "Stopped", "Value": 0, "IsSystem": False}} }}
    Wait Until Keyword Succeeds    10s    1s    Next Sample Value Should Be    pi-snap    pump_name    Stopped

Unknown Digital State Is Refused Without A Request
    ${before}=    Sim Counters    sim-snapshot
    ${r}=    Write Point    pi-snap    pump_name    "Exploded"    w-unknown
    Should Be Equal    ${{ json.loads($r)["status"] }}    failed
    Should Contain    ${{ json.loads($r)["reason"] }}    Exploded
    ${after}=    Sim Counters    sim-snapshot
    Should Be Equal    ${after.get("streams/value", 0)}    ${before.get("streams/value", 0)}

Write Refused By The Server
    ${r}=    Write Point    pi-snap    readonly    2    w-403
    Should Be Equal    ${{ json.loads($r)["status"] }}    failed
    Should Contain    ${{ json.loads($r)["reason"] }}    403
    Should Contain    ${{ json.loads($r)["reason"] }}    write access denied
    Link Should Be    pi-snap    connected

Write While The Server Fails
    Sim Post    sim-snapshot    fail    {"status": 503, "only": "write"}
    ${r}=    Write Point    pi-snap    setpoint    1.5    w-503
    Sim Post    sim-snapshot    fail    {}
    Should Be Equal    ${{ json.loads($r)["status"] }}    failed
    Should Contain    ${{ json.loads($r)["reason"] }}    503

Chunks Shrink On URI Too Long
    Sim Post    sim-snapshot    limits    {"max_web_ids": 5, "status": 414}
    Sleep    5s
    Every Point Reads Good    pi-snap    sinusoid    count16    batch    setpoint
    Sim Post    sim-snapshot    limits    {}

Chunks Shrink On A Request HTTP.sys Finds Too Long
    Sim Post    sim-snapshot    limits    {"max_url_bytes": 500, "status": 400}
    Sleep    5s
    Every Point Reads Good    pi-snap    sinusoid    count16    batch    setpoint
    Sim Post    sim-snapshot    limits    {}

Server That Stops Answering Degrades The Link
    ${mark}=    Get Message Mark
    Sim Post    sim-snapshot    pause    {"on": true}
    Wait For Link Lost    pi-snap    ${OUTAGE_TIMEOUT}    ${mark}
    ${mark}=    Get Message Mark
    Sim Post    sim-snapshot    pause    {"on": false}
    Wait For Link    pi-snap    connected    ${RECOVERY_TIMEOUT}    ${mark}

Channel Pushes A New Value With Its PI Time
    ${mark}=    Get Message Mark
    ${ev}=    Sim Post    simulator    set    {"tag": "FROZEN", "value": 8.25}
    ${s}=    Wait For Fresh Message With Field    te/device/pi/ot/${PROTOCOL}/sample/frozen    value    ${8.25}
    ...    timeout=${SAMPLE_TIMEOUT}    since=${mark}
    Same Instant    ${{ json.loads($s)["ts"] }}    ${ev}[ts]

Polled Point On A Channel Device
    Wait For Sample Of    pi    slow

Recorded Polling Delivers Every Event
    Wait For Sample Of    pi-rec    ticks
    ${mark}=    Get Message Mark
    Sim Post    sim-recorded    burst    {"tag": "FROZEN", "count": 5, "start": "*-0.6s", "step_ms": 100}
    Wait Until Keyword Succeeds    10s    1s    Samples Since Should Have Values
    ...    pi-rec    frozen    ${mark}    ${{ [0, 1, 2, 3, 4] }}

Channels Refused Falls Back To Recorded Polling
    Link Should Be    pi-fallback    connected
    Wait For Sample Of    pi-fallback    ticks

Silent Channel Is Detected
    [Documentation]    `pi-maxage` has pushed points only, so a silent channel degrades its link.
    ...    (A device that also polls points keeps its link and re-subscribes instead.)
    Wait For Sample Of    pi-maxage    ticks
    ${mark}=    Get Message Mark
    Sim Post    simulator    channels    {"silent": true}
    Wait For Link Lost    pi-maxage    ${OUTAGE_TIMEOUT}    ${mark}
    ${mark}=    Get Message Mark
    Sim Post    simulator    channels    {"silent": false}
    Wait For Link    pi-maxage    connected    ${RECOVERY_TIMEOUT}    ${mark}

Outage Gap Is Filled From The Archive
    [Documentation]    The API is down for 15 s while TICKS keeps archiving one event a second.
    ...    Device `pi` replays every one of them in order, with no duplicate; `pi-maxage`
    ...    (backfill.max_age 5s) replays only the last 5 seconds.
    Wait For Sample Of    pi    ticks
    Wait For Sample Of    pi-maxage    ticks
    ${mark}=    Get Message Mark
    Sim Post    simulator    outage    {"on": true}
    # Degraded, or disconnected once a reconnect attempt is refused (503).
    Wait For Fresh Message With Field    te/device/pi/ot/${PROTOCOL}/status/link    status    degraded    disconnected
    ...    timeout=${OUTAGE_TIMEOUT}    since=${mark}
    Sleep    15s
    ${up}=    Get Message Mark
    Sim Post    simulator    outage    {"on": false}
    Wait For Link    pi    connected    ${RECOVERY_TIMEOUT}    ${up}
    Wait For Link    pi-maxage    connected    ${RECOVERY_TIMEOUT}    ${up}
    Sleep    5s
    ${values}=    Sample Values Since    pi    ticks    ${mark}
    Should Be Contiguous    ${values}
    # From 3 s before the outage, so the last values before it are included.
    ${before}=    Evaluate    ${mark} - 3
    ${bounded}=    Sample Values Since    pi-maxage    ticks    ${before}
    ${missing}=    Evaluate    max($bounded) - min($bounded) + 1 - len(set($bounded))
    Should Be True    ${missing} >= 5    msg=max_age 5s should have skipped most of a 15 s gap: ${bounded}

Restart Catches Up From The Cursor File
    Wait For Sample Of    pi    ticks
    ${mark}=    Get Message Mark
    Stop Stack Service    connector
    Sleep    5s
    Start Stack Service    connector
    Wait For Link    pi    connected    ${RECOVERY_TIMEOUT}    ${mark}
    Sleep    3s
    ${values}=    Sample Values Since    pi    ticks    ${mark}
    Should Be Contiguous    ${values}

Large Backfill Loses Nothing
    [Documentation]    5000 events archived while the connector was stopped, after its cursor,
    ...    are all published (paged recorded reads, paced by the runtime).
    ${start}    ${mark}=    Stop Connector With Cursor At    burst_all    SETPOINT    700
    Sim Post    simulator    burst    {"tag": "SETPOINT", "count": 5000, "start": "${start}", "step_ms": 100}
    Start Stack Service    connector
    Wait Until Keyword Succeeds    60s    2s    Samples In Window Should Number
    ...    pi    burst_all    ${mark}    ${start}    5000    5000

Backfill Keeps The Reporting Rate Of Source Time
    [Documentation]    600 events one second apart, replayed in a burst to a point with
    ...    `min_interval = "10s"`: about one per 10 s of source time, not 2 (contract §5.3 rule 7).
    ${start}    ${mark}=    Stop Connector With Cursor At    burst_limited    COUNT16    700
    Sim Post    simulator    burst    {"tag": "COUNT16", "count": 600, "start": "${start}", "step_ms": 1000}
    Start Stack Service    connector
    Wait Until Keyword Succeeds    60s    2s    Samples In Window Should Number
    ...    pi    burst_limited    ${mark}    ${start}    55    65

Basic Auth With A Wrong Password Is Unauthorized
    ${payload}=    Wait For Retained    te/device/pi-badpass/ot/${PROTOCOL}/status/link    timeout=${READY_TIMEOUT}
    Should Be Equal    ${{ json.loads($payload)["status"] }}    disconnected
    Should Be Equal    ${{ json.loads($payload)["reason"] }}    unauthorized

Bearer Token Is Re-read After Rotation
    Wait For Sample Of    pi-bearer    ticks
    ${mark}=    Get Message Mark
    Sim Post    sim-bearer    token    {"token": "token-2"}
    DeviceLibrary.Execute Command    cmd=sh -c 'printf "token-2\\n" > /etc/tedge-dot/secrets/token'
    Wait For Fresh Message With Field    te/device/pi-bearer/ot/${PROTOCOL}/sample/ticks    quality    good
    ...    timeout=${SAMPLE_TIMEOUT}    since=${mark}
    Link Should Stay Connected    pi-bearer    ${mark}

Client Certificate Authentication
    Link Should Be    pi-mtls    connected
    # Longer than one poll: the tests before this one restart the connector three times, and the
    # C runtime serves its devices one at a time while they catch up.
    Wait For Sample    te/device/pi-mtls/ot/${PROTOCOL}/sample/ticks    timeout=30

Certificate Without A SAN Is Refused
    TLS Failure Should Be Named    pi-nosan

Untrusted Server Certificate Is Refused
    TLS Failure Should Be Named    pi-untrusted


Measurement Output Stamps A Pushed Value With Its PI Time
    [Documentation]    In measurement output the connector publishes the thin-edge measurement
    ...                itself, with the time PI archived the value (contract §5.4, §5.5).
    Wait For Message Containing    ${MEASURE_TOPIC}    "ticks"    timeout=${READY_TIMEOUT}
    ${ev}=    Sim Post    sim-measure    set    {"tag": "FROZEN", "value": 4.75}
    ${payload}=    Wait For Message Containing    ${MEASURE_TOPIC}    4.75    timeout=${SAMPLE_TIMEOUT}
    ${value}=    Get Json Field    ${payload}    ${PROTOCOL}.frozen
    Should Be Equal As Numbers    ${value}    4.75
    Same Instant    ${{ json.loads($payload)["time"] }}    ${ev}[ts]

Measurement Output Names Series As The Measurement Flow Does
    [Documentation]    A point's meta.measurement names its group and series.
    Sim Post    sim-measure    set    {"tag": "FROZEN", "value": 6.5}
    ${payload}=    Wait For Message Containing    te/device/pim///m/Tank    6.5    timeout=${SAMPLE_TIMEOUT}
    ${value}=    Get Json Field    ${payload}    Tank.Level
    Should Be Equal As Numbers    ${value}    6.5

Measurement Output Publishes The Units Metadata
    [Documentation]    The units of a measurement topic, retained on <topic>/meta: only the
    ...                series with a unit.
    Wait For Message Containing    ${MEASURE_TOPIC}    "time"    timeout=${READY_TIMEOUT}
    ${payload}=    Fetch Retained    ${MEASURE_TOPIC}/meta    timeout=${SAMPLE_TIMEOUT}
    # The keys hold a dot ("<group>.<series>"), so they are matched as text.
    Should Contain    ${payload}    "${PROTOCOL}.frozen":{"unit":"m"}
    Should Not Contain    ${payload}    ticks

Measurement Output Publishes No Samples For Plain Points
    Wait For Message Containing    ${MEASURE_TOPIC}    "ticks"    timeout=${READY_TIMEOUT}
    No New Messages On Topic    te/device/pim/ot/${PROTOCOL}/sample/ticks    timeout=5

Measurement Output Replays An Outage With The PI Times
    [Documentation]    TICKS archives one event a second while sim-measure is down. After the
    ...                outage every event arrives as a measurement of its own, in order, with no
    ...                gap or duplicate, each stamped with its PI time: replayed samples are not
    ...                batched under the time of the catch-up.
    Wait For Message Containing    ${MEASURE_TOPIC}    "ticks"    timeout=${READY_TIMEOUT}
    ${mark}=    Get Message Mark
    Sim Post    sim-measure    outage    {"on": true}
    Wait For Link Lost    pim    ${OUTAGE_TIMEOUT}    ${mark}
    Sleep    8s
    ${up}=    Get Message Mark
    Sim Post    sim-measure    outage    {"on": false}
    Wait For Link    pim    connected    ${RECOVERY_TIMEOUT}    ${up}
    Sleep    5s
    ${all}=    Get Messages Since    ${MEASURE_TOPIC}    ${mark}
    ${ticks}=    Evaluate    [json.loads(p) for p in $all if "ticks" in json.loads(p).get($PROTOCOL, {})]
    ${values}=    Evaluate    [m[$PROTOCOL]["ticks"] for m in $ticks]
    Should Be Contiguous    ${values}
    Should Be True    len($values) >= 8    msg=an 8 s outage should replay at least 8 events: ${values}
    ${times}=    Evaluate    [datetime.datetime.fromisoformat(m["time"].replace("Z", "+00:00")).timestamp() for m in $ticks]    modules=datetime
    ${steps}=    Evaluate    [round(b - a, 1) for a, b in zip($times, $times[1:])]
    Should Be True    all(0.5 <= d <= 1.5 for d in $steps)    msg=PI times one second apart expected: ${steps}

*** Keywords ***
Setup PI Stack
    Setup OT Stack    ${PROTOCOL}
    Wait For Fresh Message With Field    te/device/pi/ot/${PROTOCOL}/status/link    status    connected
    ...    timeout=${READY_TIMEOUT}    since=0
    Wait For Fresh Message With Field    te/device/pi-snap/ot/${PROTOCOL}/status/link    status    connected
    ...    timeout=${READY_TIMEOUT}    since=0

Sim Curl
    [Documentation]    Run curl against a simulator's own port, inside its container.
    [Arguments]    ${service}    ${method}    ${path}    ${body}=${EMPTY}
    ${tls}=    Evaluate    $service not in ("sim-snapshot", "sim-recorded", "sim-refuse", "sim-measure")
    ${url}=    Set Variable If    ${tls}    https://localhost:8443${path}    http://localhost:8080${path}
    ${cert}=    Set Variable If    "${service}" == "sim-mtls"    --cert /certs/client.pem --key /certs/client.key    ${EMPTY}
    ${data}=    Set Variable If    $body    -H 'Content-Type: application/json' -d '${body}'    ${EMPTY}
    ${project}=    DeviceLibrary.Get Compose Project Name
    # -k on timeout: `docker compose exec` ignores a plain SIGTERM while attached.
    ${rc}    ${out}=    Run And Return Rc And Output
    ...    timeout -k 5 30 docker compose -p ${project} exec -T ${service} curl -sk ${cert} -X ${method} ${data} '${url}'
    Should Be Equal As Integers    ${rc}    0    msg=${out}
    RETURN    ${out}

Sim Post
    [Arguments]    ${service}    ${what}    ${body}={}
    ${out}=    Sim Curl    ${service}    POST    /_sim/${what}    ${body}
    RETURN    ${{ json.loads($out) }}

Sim Get
    [Arguments]    ${service}    ${what}
    ${out}=    Sim Curl    ${service}    GET    /_sim/${what}
    RETURN    ${{ json.loads($out) }}

Sim Counters
    [Arguments]    ${service}
    ${c}=    Sim Get    ${service}    counters
    RETURN    ${c}

Web Id Of
    [Arguments]    ${tag}
    ${project}=    DeviceLibrary.Get Compose Project Name
    ${rc}    ${out}=    Run And Return Rc And Output
    ...    timeout -k 5 30 docker compose -p ${project} exec -T simulator curl -sk -u piuser:pi-password-1 'https://localhost:8443/piwebapi/points?path=%5C%5CPISRV01%5C${tag}&selectedFields=WebId'
    Should Be Equal As Integers    ${rc}    0    msg=${out}
    RETURN    ${{ json.loads($out)["WebId"] }}

Wait For Sample Of
    [Arguments]    ${device}    ${point}
    ${s}=    Wait For Sample    te/device/${device}/ot/${PROTOCOL}/sample/${point}    timeout=${SAMPLE_TIMEOUT}
    RETURN    ${s}

Next Sample Value Should Be
    [Arguments]    ${device}    ${point}    ${expected}
    ${s}=    Wait For Sample Of    ${device}    ${point}
    ${v}=    Get Json Field    ${s}    value
    Should Be Equal As Strings    ${v}    ${expected}

Write Point
    [Documentation]    Publish a `write` command (value as JSON text) and return its final result.
    [Arguments]    ${device}    ${point}    ${value}    ${id}
    ${topic}=    Set Variable    te/device/${device}/ot/${PROTOCOL}/cmd/write/${id}
    Publish Message    ${topic}    {"status":"init","point":"${point}","value":${value}}    retain=True
    ${result}=    Wait For Fresh Message With Field    ${topic}    status    successful    failed
    ...    timeout=${SAMPLE_TIMEOUT}    since=0
    RETURN    ${result}

Every Point Reads Good
    [Arguments]    ${device}    @{points}
    FOR    ${p}    IN    @{points}
        ${s}=    Wait For Sample Of    ${device}    ${p}
        Should Be Equal    ${{ json.loads($s)["quality"] }}    good    msg=${p}: ${s}
    END

Link Should Be
    [Arguments]    ${device}    ${status}
    ${payload}=    Wait For Retained    te/device/${device}/ot/${PROTOCOL}/status/link    timeout=${READY_TIMEOUT}
    Should Be Equal    ${{ json.loads($payload)["status"] }}    ${status}    msg=${payload}

Wait For Link
    [Arguments]    ${device}    ${status}    ${timeout}    ${since}
    Wait For Fresh Message With Field    te/device/${device}/ot/${PROTOCOL}/status/link    status    ${status}
    ...    timeout=${timeout}    since=${since}

Wait For Link Lost
    [Documentation]    `degraded`, or `disconnected` (the C runtime's state for a failed read, and
    ...    either runtime's once a reconnect attempt fails).
    [Arguments]    ${device}    ${timeout}    ${since}
    Wait For Fresh Message With Field    te/device/${device}/ot/${PROTOCOL}/status/link    status    degraded    disconnected
    ...    timeout=${timeout}    since=${since}

Link Should Stay Connected
    [Documentation]    No link status other than `connected` for `device` since `mark`, over 5 s.
    [Arguments]    ${device}    ${mark}
    Sleep    5s
    ${since}=    Get Messages Since    te/device/${device}/ot/${PROTOCOL}/status/link    ${mark}
    FOR    ${p}    IN    @{since}
        Should Be Equal    ${{ json.loads($p)["status"] }}    connected    msg=${p}
    END

TLS Failure Should Be Named
    [Arguments]    ${device}
    ${payload}=    Wait For Retained    te/device/${device}/ot/${PROTOCOL}/status/link    timeout=${READY_TIMEOUT}
    Should Not Be Equal    ${{ json.loads($payload)["status"] }}    connected
    ${reason}=    Evaluate    json.loads($payload).get("reason", "").lower()
    Should Contain    ${reason}    certificate    msg=the reason should name the TLS failure: ${payload}

Same Instant
    [Documentation]    Two RFC 3339 times at millisecond precision are the same instant.
    [Arguments]    ${a}    ${b}
    ${da}=    Evaluate    datetime.datetime.fromisoformat($a.replace("Z", "+00:00")).timestamp()    modules=datetime
    ${db}=    Evaluate    datetime.datetime.fromisoformat($b.replace("Z", "+00:00")).timestamp()    modules=datetime
    Should Be True    abs(${da} - ${db}) < 0.002    msg=${a} != ${b}

Sample Values Since
    [Documentation]    The values of the good samples of a point received after `mark`, in order.
    [Arguments]    ${device}    ${point}    ${mark}
    ${all}=    Get Messages Since    te/device/${device}/ot/${PROTOCOL}/sample/${point}    ${mark}
    ${values}=    Evaluate    [json.loads(p)["value"] for p in $all if json.loads(p)["quality"] == "good"]
    RETURN    ${values}

Should Be Contiguous
    [Documentation]    Integer values (TICKS) with no gap and no duplicate.
    [Arguments]    ${values}
    ${ints}=    Evaluate    [int(v) for v in $values]
    Should Not Be Empty    ${ints}
    ${expected}=    Evaluate    list(range($ints[0], $ints[0] + len($ints)))
    Should Be Equal    ${ints}    ${expected}    msg=gap or duplicate in ${ints}

Samples Since Should Have Values
    [Arguments]    ${device}    ${point}    ${mark}    ${expected}
    ${all}=    Get Messages Since    te/device/${device}/ot/${PROTOCOL}/sample/${point}    ${mark}
    ${values}=    Evaluate    [json.loads(p)["value"] for p in $all]
    FOR    ${v}    IN    @{expected}
        List Should Contain Value    ${values}    ${v}
    END

Stop Connector With Cursor At
    [Documentation]    Stop the connector and give `point` of device `pi` a cursor `age` seconds
    ...    in the past (written into the stopped container), so the history archived after it
    ...    is replayed on the next start. Returns the first event time to archive (one second
    ...    after the cursor) and a message mark.
    [Arguments]    ${point}    ${tag}    ${age}
    ${web_id}=    Web Id Of    ${tag}
    ${mark}=    Get Message Mark
    Stop Stack Service    connector
    ${now}=    Evaluate    time.time()    modules=time
    ${cursor}=    Evaluate    datetime.datetime.fromtimestamp(${now} - ${age}, datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.000Z")    modules=datetime
    ${start}=    Evaluate    datetime.datetime.fromtimestamp(${now} - ${age} + 1, datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.000Z")    modules=datetime
    ${file}=    Set Variable    ${TEMPDIR}/pi-cursor-${point}.json
    Create File    ${file}    {"version":1,"points":{"${point}":{"web_id":"${web_id}","ts":"${cursor}","values":[-1]}}}
    Run Compose    cp ${file} connector:${STATE_DIR}/pi.json
    RETURN    ${start}    ${mark}

Samples In Window Should Number
    [Documentation]    Between `low` and `high` samples of a point, received after the mark, whose
    ...    time lies in the 650 s from `start`: replayed history, not live data.
    [Arguments]    ${device}    ${point}    ${mark}    ${start}    ${low}    ${high}
    ${all}=    Get Messages Since    te/device/${device}/ot/${PROTOCOL}/sample/${point}    ${mark}
    ${t0}=    Evaluate    datetime.datetime.fromisoformat($start.replace("Z", "+00:00")).timestamp()    modules=datetime
    # Burst event i carries the value i: count those, not other archived events in the window.
    ${n}=    Evaluate    len([p for p in $all if 0 <= datetime.datetime.fromisoformat(json.loads(p)["ts"].replace("Z", "+00:00")).timestamp() - ${t0} < 650 and float(json.loads(p).get("value", -1)).is_integer() and json.loads(p).get("value", -1) >= 0])    modules=datetime,json
    Should Be True    ${low} <= ${n} <= ${high}    msg=${n} replayed samples, expected ${low}..${high}
