*** Settings ***
Documentation       Full end-to-end test for tedge-dot against Cumulocity.
...                 The tedge container installs the connector, its thin-edge flows and the
...                 Cumulocity operation shims (no Python plugin), reads a real Modbus simulator,
...                 and bridges everything to Cumulocity. These tests assert the cloud-facing
...                 behaviour: child registration, the device twin, measurements (get), and
...                 c8y_SetRegister / c8y_SetCoil operations (set).
...
...                 Requires C8Y_BASEURL / C8Y_USER / C8Y_PASSWORD / C8Y_TENANT and DEVICE_ID,
...                 plus a running stack (see just test-e2e-c8y).

Resource            ../../_shared/device.resource
Library             Collections

# Measurement assertions filter with value=<fragment> (valueFragmentType), not fragment=
# (fragmentType): Cumulocity rejects fragmentType combined with valueFragmentSeries with an
# HTTP 500 "Value Fragment filter already provided" (observed 2026-09-11).

Suite Setup         Setup Main Device Context
Suite Teardown      Teardown Cloud Device


*** Variables ***
${CHILD_NAME}           plc1
# ${CHILD_EXTERNAL_ID} is built in the suite setup: it embeds the per-run device id.

${MEAS_TIMEOUT}         60
${OP_TIMEOUT}           30


*** Test Cases ***
Connector Service Is Registered
    [Documentation]    The connector runs as a tedge service on the main device.
    Set Main Device
    Cumulocity.Should Have Services    name=tedge-dot-modbus    min_count=1    timeout=${MEAS_TIMEOUT}

Child Device Is Registered
    [Documentation]    The connector's device is auto-registered as a modbus child device.
    Cumulocity.Device Should Exist    ${CHILD_EXTERNAL_ID}

Child Device Has Modbus Twin Fragment
    [Documentation]    ot-registration publishes the connector descriptor as c8y_ModbusDevice.
    Cumulocity.Device Should Exist    ${CHILD_EXTERNAL_ID}
    ${mo}=    Managed Object Should Have Fragments    c8y_ModbusDevice
    Should Be Equal    ${mo}[c8y_ModbusDevice][protocol]    modbus
    Should Be Equal    ${mo}[c8y_ModbusDevice][transport]    tcp

Measurements Are Sent To Cumulocity
    [Documentation]    The uint16 holding register (17001) arrives as a modbus measurement.
    Cumulocity.Device Should Exist    ${CHILD_EXTERNAL_ID}
    Cumulocity.Device Should Have Measurements
    ...    minimum=1    type=modbus    value=modbus    series=temp_u16    timeout=${MEAS_TIMEOUT}

Set Register Operation Round-Trips
    [Documentation]    c8y_SetRegister writes 4242 to temp_u16; the next reading reflects it.
    # Cumulocity.Execute Shell Command    text=tedge mqtt pub te/device/plc1//cmd/
    Cumulocity.Device Should Exist    ${CHILD_EXTERNAL_ID}
    ${operation}=    Cumulocity.Create Operation
    ...    fragments={"c8y_SetRegister":{"point":"temp_u16","value":4242}}
    ...    description=Set temp_u16 to 4242
    Cumulocity.Operation Should Be SUCCESSFUL    ${operation}    timeout=${OP_TIMEOUT}
    ${measurements}=    Cumulocity.Device Should Have Measurements
    ...    minimum=1    type=modbus    value=modbus    series=temp_u16
    ...    sort_newest=${True}    timeout=${MEAS_TIMEOUT}
    Should Be Equal As Integers    ${measurements[0]["modbus"]["temp_u16"]["value"]}    4242

Set Coil Operation Round-Trips
    [Documentation]    c8y_SetCoil sets coil_rw true; the next reading reflects it.
    Cumulocity.Device Should Exist    ${CHILD_EXTERNAL_ID}
    ${operation}=    Cumulocity.Create Operation
    ...    fragments={"c8y_SetCoil":{"point":"coil_rw","value":true}}
    ...    description=Set coil_rw true
    Cumulocity.Operation Should Be SUCCESSFUL    ${operation}    timeout=${OP_TIMEOUT}
    # The newest reading, not the first returned (the library sorts oldest first unless told
    # otherwise), and retried: a reading taken just before the write can still be the newest.
    Wait Until Keyword Succeeds    ${MEAS_TIMEOUT}s    2s
    ...    Newest Coil Reading Should Be    1.0


*** Keywords ***
Newest Coil Reading Should Be
    [Arguments]    ${expected}
    ${measurements}=    Cumulocity.Device Should Have Measurements
    ...    minimum=1    type=modbus    value=modbus    series=coil_rw
    ...    sort_newest=${True}    timeout=${MEAS_TIMEOUT}
    Should Be Equal As Numbers    ${measurements[0]["modbus"]["coil_rw"]["value"]}    ${expected}    precision=1

Setup Main Device Context
    Setup Cloud Device
    Set Suite Variable    $CHILD_EXTERNAL_ID    ${DEVICE_ID}:device:${CHILD_NAME}
    Set Main Device

Set Main Device
    Cumulocity.Set Device    ${DEVICE_ID}
