*** Settings ***
Documentation       Full end-to-end test of the AVEVA PI Web API connector against Cumulocity.
...                 The tedge container installs the connector and its thin-edge flows, reads the
...                 PI Web API simulator over HTTPS with Basic auth, and bridges everything to
...                 Cumulocity. These tests assert the cloud-facing behaviour: child registration,
...                 the device twin, measurements stamped with the time PI archived the value,
...                 an outage and a connector restart filled from the PI archive with the original
...                 times, and device parameters written back to PI (c8y_ParameterUpdate).
...
...                 The tests run in order on one device: the parameter definition is registered
...                 before it is used, and the outage and restart run last.
...
...                 Requires C8Y_BASEURL / C8Y_USER / C8Y_PASSWORD (C8Y_TENANT unless C8Y_USER
...                 is `tenant/user`) in the environment or .env, and a tenant with the dtm and
...                 device-parameter microservices. Run with `just test-cloud aveva-pi` (after
...                 `just build`) or `just test-cloud-c aveva-pi`.

Resource            ../../_shared/device.resource
Library             Collections
Library             DateTime
Library             ../../_shared/ParameterLibrary.py

# Measurement assertions filter with value=<fragment> (valueFragmentType), not fragment=
# (fragmentType): see cloud/modbus/tests/modbus_c8y.robot.

Suite Setup         Setup Child Context
Suite Teardown      Teardown Cloud Device


*** Variables ***
${CHILD_NAME}           pi
# ${CHILD_EXTERNAL_ID} is built in the suite setup: it embeds the per-run device id.
${GROUP}                aveva-pi
# Named after the device type of aveva-pi.toml (§5.2).
${SET}                  pi_plant_sim_control_parameters
${SIM}                  https://simulator:8443
${CA}                   /opt/pi-sim/certs/ca.pem

${MEAS_TIMEOUT}         90
${OP_TIMEOUT}           60
# How long the PI Web API is down, and the connector stopped: TICKS archives one event a second
# meanwhile.
${GAP}                  20


*** Test Cases ***
Connector Service Is Registered
    [Documentation]    The connector runs as a tedge service on the main device.
    Cumulocity.Set Device    ${DEVICE_ID}
    Cumulocity.Should Have Services    name=tedge-dot-aveva-pi    min_count=1    timeout=${MEAS_TIMEOUT}

Child Device Is Registered
    Cumulocity.Device Should Exist    ${CHILD_EXTERNAL_ID}    timeout=${MEAS_TIMEOUT}

Child Device Carries The Connector Descriptor
    [Documentation]    ot-registration publishes the link status `info` as c8y_AvevaPiDevice.
    Cumulocity.Device Should Exist    ${CHILD_EXTERNAL_ID}
    ${mo}=    Managed Object Should Have Fragments    c8y_AvevaPiDevice    timeout=${MEAS_TIMEOUT}
    Should Be Equal    ${mo}[c8y_AvevaPiDevice][base_url]    https://simulator:8443/piwebapi
    Should Be Equal    ${mo}[c8y_AvevaPiDevice][delivery]    channel

Measurements Are Sent To Cumulocity
    Cumulocity.Device Should Exist    ${CHILD_EXTERNAL_ID}
    Cumulocity.Device Should Have Measurements
    ...    minimum=3    type=${GROUP}    value=${GROUP}    series=ticks    timeout=${MEAS_TIMEOUT}
    Cumulocity.Device Should Have Measurements
    ...    minimum=1    type=${GROUP}    value=${GROUP}    series=pressure    timeout=${MEAS_TIMEOUT}

Measurement Units Come From PI
    [Documentation]    SINUSOID has the PI unit degC, the AF attribute bar; neither is configured.
    ...                The first measurement may still go without it (the mapper learns the unit
    ...                through the broker), so the newest one is checked.
    Cumulocity.Device Should Exist    ${CHILD_EXTERNAL_ID}
    Wait Until Keyword Succeeds    ${MEAS_TIMEOUT}s    2s    Newest Measurement Unit Should Be    sinusoid    degC
    Wait Until Keyword Succeeds    ${MEAS_TIMEOUT}s    2s    Newest Measurement Unit Should Be    pressure    bar

Measurement Time Is The PI Time
    [Documentation]    A measurement carries the time PI archived the value (Channels push), not
    ...                the time the connector received it: the newest TICKS measurement has the
    ...                timestamp of the archived event with the same value.
    Cumulocity.Device Should Exist    ${CHILD_EXTERNAL_ID}
    ${measurements}=    Cumulocity.Device Should Have Measurements
    ...    minimum=1    type=${GROUP}    value=${GROUP}    series=ticks    sort_newest=${True}    timeout=${MEAS_TIMEOUT}
    ${value}=    Set Variable    ${measurements[0]["${GROUP}"]["ticks"]["value"]}
    ${archive}=    Sim Get    archive?tag=TICKS
    ${events}=    Evaluate    [e for e in $archive if e["value"] == int($value)]
    Should Not Be Empty    ${events}    msg=TICKS ${value} is not in the PI archive
    Same Instant    ${measurements[0]}[time]    ${events[0]}[ts]

Parameter Definitions Are Rendered From The Connector Config
    [Documentation]    `tedge-dot describe` renders one DTM property definition for the device
    ...                type, with the writable points as properties.
    ${output}=    DeviceLibrary.Execute Command
    ...    cmd=tedge-dot describe -c /etc/tedge/plugins/ot/aveva-pi.toml --compact
    ${definition}=    Evaluate    json.loads([l for l in $output.splitlines() if l.startswith("{")][0])    modules=json
    Should Be Equal    ${definition}[identifier]    ${SET}
    Dictionary Should Contain Key    ${definition}[jsonSchema][properties]    setpoint
    Dictionary Should Contain Key    ${definition}[jsonSchema][properties]    locked
    Dictionary Should Not Contain Key    ${definition}[jsonSchema][properties]    ticks
    Should Be Equal    ${definition}[jsonSchema][properties][setpoint][title]    Setpoint
    Set Suite Variable    ${DEFINITION}    ${definition}

Parameter Definition Is Registered In The Tenant
    Ensure DTM Property Definition    ${DEFINITION}
    DTM Property Definition Should Match    ${DEFINITION}

Child Device Carries The Parameter Set Fragment
    [Documentation]    ot-parameter-state publishes the writable points as a twin fragment with
    ...                the values PI holds.
    Cumulocity.Device Should Exist    ${CHILD_EXTERNAL_ID}
    Cumulocity.Managed Object Should Have Fragment Values
    ...    ${SET}.setpoint\=21.5    ${SET}.locked\=1    timeout=${MEAS_TIMEOUT}

Parameter Update Writes The Value To PI
    [Documentation]    The Parameters tab's operation is one connector write-batch: PI receives
    ...                the value, the operation completes, and the fragment and the next
    ...                measurement follow.
    Cumulocity.Device Should Exist    ${CHILD_EXTERNAL_ID}
    ${operation}=    Cumulocity.Create Operation
    ...    fragments={"c8y_ParameterUpdate":{},"c8y_ParameterUpdate_${SET}":{},"${SET}":{"setpoint":42.5}}
    ...    description=Update ${SET}
    Cumulocity.Operation Should Be SUCCESSFUL    ${operation}    timeout=${OP_TIMEOUT}
    ${writes}=    Sim Get    writes
    Should Be Equal As Numbers    ${writes}[SETPOINT][body][Value]    42.5
    Cumulocity.Managed Object Should Have Fragment Values    ${SET}.setpoint\=42.5    timeout=${MEAS_TIMEOUT}
    Wait Until Keyword Succeeds    ${MEAS_TIMEOUT}s    2s    Newest Measurement Should Be    setpoint    42.5

Parameter Update Refused By PI Fails With Its Reason
    [Documentation]    PI refuses writes to READONLY (403). The operation fails with the
    ...                server's status, and the fragment keeps the value PI holds.
    Cumulocity.Device Should Exist    ${CHILD_EXTERNAL_ID}
    ${operation}=    Cumulocity.Create Operation
    ...    fragments={"c8y_ParameterUpdate":{},"c8y_ParameterUpdate_${SET}":{},"${SET}":{"locked":5}}
    ...    description=Update ${SET} (refused)
    ${op}=    Cumulocity.Operation Should Be FAILED    ${operation}    timeout=${OP_TIMEOUT}
    Should Contain    ${op}[failureReason]    403
    Should Contain    ${op}[failureReason]    write access denied
    Cumulocity.Managed Object Should Have Fragment Values    ${SET}.locked\=1    timeout=${MEAS_TIMEOUT}

Outage Is Filled From The PI Archive With The Original Times
    [Documentation]    The PI Web API is down for ${GAP} s while TICKS keeps archiving. After the
    ...                outage every event of the gap reaches Cumulocity as a measurement stamped
    ...                with its PI time: contiguous values, none missing, none twice.
    Cumulocity.Device Should Exist    ${CHILD_EXTERNAL_ID}
    ${start}=    Get Current Date    UTC    result_format=%Y-%m-%dT%H:%M:%S.%fZ
    Sim Post    outage    {"on": true}
    Sleep    ${GAP}s
    ${end}=    Get Current Date    UTC    result_format=%Y-%m-%dT%H:%M:%S.%fZ
    Sim Post    outage    {"on": false}
    Wait Until Keyword Succeeds    ${MEAS_TIMEOUT}s    5s    Ticks Between Should Be Contiguous    ${start}    ${end}

Restart Catches Up From The Cursor
    [Documentation]    The connector service is stopped for ${GAP} s. On start it reads the gap
    ...                from the PI archive, from the cursor it persisted under
    ...                /var/lib/tedge-dot/aveva-pi.
    Cumulocity.Device Should Exist    ${CHILD_EXTERNAL_ID}
    ${start}=    Get Current Date    UTC    result_format=%Y-%m-%dT%H:%M:%S.%fZ
    DeviceLibrary.Execute Command    cmd=systemctl stop tedge-dot
    Sleep    ${GAP}s
    ${end}=    Get Current Date    UTC    result_format=%Y-%m-%dT%H:%M:%S.%fZ
    DeviceLibrary.Execute Command    cmd=systemctl start tedge-dot
    Wait Until Keyword Succeeds    ${MEAS_TIMEOUT}s    5s    Ticks Between Should Be Contiguous    ${start}    ${end}


*** Keywords ***
Setup Child Context
    Setup Cloud Device    protocol=aveva-pi
    Set Suite Variable    $CHILD_EXTERNAL_ID    ${DEVICE_ID}:device:${CHILD_NAME}
    Cumulocity.Set Device    ${DEVICE_ID}

Sim Get
    [Documentation]    GET the simulator's control API (`/_sim/<what>`) from the tedge container.
    [Arguments]    ${what}
    ${out}=    DeviceLibrary.Execute Command
    ...    cmd=curl -sSf --cacert ${CA} '${SIM}/_sim/${what}'    strip=${True}    log_output=${False}
    RETURN    ${{ json.loads($out) }}

Sim Post
    [Arguments]    ${what}    ${body}={}
    ${out}=    DeviceLibrary.Execute Command
    ...    cmd=curl -sSf --cacert ${CA} -X POST -H 'Content-Type: application/json' -d '${body}' '${SIM}/_sim/${what}'
    ...    strip=${True}
    RETURN    ${{ json.loads($out) }}

Same Instant
    [Documentation]    Two RFC 3339 times at millisecond precision are the same instant.
    [Arguments]    ${a}    ${b}
    ${da}=    Evaluate    datetime.datetime.fromisoformat($a.replace("Z", "+00:00")).timestamp()    modules=datetime
    ${db}=    Evaluate    datetime.datetime.fromisoformat($b.replace("Z", "+00:00")).timestamp()    modules=datetime
    Should Be True    abs(${da} - ${db}) < 0.002    msg=${a} != ${b}

Newest Measurement Unit Should Be
    [Arguments]    ${series}    ${unit}
    ${measurements}=    Cumulocity.Device Should Have Measurements
    ...    minimum=1    type=${GROUP}    value=${GROUP}    series=${series}    sort_newest=${True}
    Should Be Equal    ${measurements[0]["${GROUP}"]["${series}"]["unit"]}    ${unit}

Newest Measurement Should Be
    [Arguments]    ${series}    ${expected}
    ${measurements}=    Cumulocity.Device Should Have Measurements
    ...    minimum=1    type=${GROUP}    value=${GROUP}    series=${series}    sort_newest=${True}
    Should Be Equal As Numbers    ${measurements[0]["${GROUP}"]["${series}"]["value"]}    ${expected}

Ticks Between Should Be Contiguous
    [Documentation]    The TICKS measurements whose time lies between `start` and `end` are one
    ...                per archived second, with no gap and no duplicate.
    [Arguments]    ${start}    ${end}
    ${measurements}=    Cumulocity.Device Should Have Measurements
    ...    minimum=1    type=${GROUP}    value=${GROUP}    series=ticks    after=${start}    before=${end}
    ${values}=    Evaluate    [int(m["${GROUP}"]["ticks"]["value"]) for m in $measurements]
    ${expected}=    Evaluate    list(range($values[0], $values[0] + len($values)))
    Should Be Equal    ${values}    ${expected}    msg=gap or duplicate in ${values}
    Should Be True    len($values) >= ${GAP} - 2    msg=${GAP} s of TICKS expected, got ${values}
