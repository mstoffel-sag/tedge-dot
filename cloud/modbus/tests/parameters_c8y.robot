*** Settings ***
Documentation       Device parameters round-trip (RFC 0003): the connector's writable points are
...                 declared in the tenant as Digital Twin Manager property definitions, show up on
...                 the child device as a twin fragment kept current by the ot-parameter-state
...                 flow, and an edit from the Cumulocity "Parameters" tab (a c8y_ParameterUpdate
...                 operation) is written to the PLC through one connector write-batch, completes
...                 the operation, and is reflected in the fragment, a c8y_ParameterUpdate event
...                 and the next measurement.
...
...                 Requires C8Y_BASEURL / C8Y_USER / C8Y_PASSWORD / C8Y_TENANT and DEVICE_ID, a
...                 tenant with the dtm + device-parameter microservices, and a running stack
...                 (see `just test-cloud modbus`).

Resource            ../../_shared/device.resource
Library             Collections
Library             ../../_shared/ParameterLibrary.py

Suite Setup         Setup Child Context
Suite Teardown      Teardown Cloud Device


*** Variables ***
${CHILD_NAME}           plc1
# ${CHILD_EXTERNAL_ID} is built in the suite setup: it embeds the per-run device id.
# The parameter set is named after the device *type* the config declares (§5.2), not after the
# protocol: a DTM identifier is tenant-wide, and two Modbus device types must not share one.
${SET}                  modbus_plc_sim_control_parameters
# A literal parameter (§5.2 meta.parameter.fragment): the point's value IS this fragment.
${LITERAL}              plc_sim_pump_enabled
${OP_TIMEOUT}           60
${MEAS_TIMEOUT}         90
${NEW_VALUE}            4343


*** Test Cases ***
Parameter Definitions Are Rendered From The Connector Config
    [Documentation]    `tedge-dot describe` renders one DTM property definition per parameter
    ...                set, with the writable points as properties (an admin registers it once).
    ${output}=    Execute Shell Command And Get Output
    ...    tedge-dot describe -c /etc/tedge/plugins/ot/modbus.toml --compact    timeout=${OP_TIMEOUT}
    # The first JSON line, not the first line: a warning on stderr (§5.2) can be interleaved.
    ${definition}=    Evaluate    json.loads([l for l in $output.splitlines() if l.startswith("{")][0])    modules=json
    Should Be Equal    ${definition}[identifier]    ${SET}
    Dictionary Should Contain Key    ${definition}[jsonSchema][properties]    temp_u16
    Dictionary Should Contain Key    ${definition}[jsonSchema][properties]    coil_rw
    Dictionary Should Not Contain Key    ${definition}[jsonSchema][properties]    level_f32
    Should Be Equal    ${definition}[jsonSchema][properties][temp_u16][title]    Temperature setpoint
    # A mapped point (§4.3) is a parameter of its labels, offered as a choice.
    Should Be Equal    ${definition}[jsonSchema][properties][state][type]    string
    Should Be Equal    ${definition}[jsonSchema][properties][state][enum]    ${{["running", "idle"]}}
    Set Suite Variable    ${DEFINITION}    ${definition}
    # The literal is in no set: it has a definition of its own.
    Dictionary Should Not Contain Key    ${definition}[jsonSchema][properties]    pump_enabled
    ${literal}=    Evaluate
    ...    [d for d in (json.loads(l) for l in $output.splitlines() if l.startswith("{")) if d["identifier"] == $LITERAL][0]
    ...    modules=json
    Should Be Equal    ${literal}[jsonSchema][type]    boolean
    Should Be Equal    ${literal}[jsonSchema][title]    Pump enabled
    Dictionary Should Not Contain Key    ${literal}[jsonSchema]    properties
    Set Suite Variable    ${LITERAL_DEFINITION}    ${literal}

Parameter Definition Is Registered In The Tenant
    [Documentation]    The rendered definition is posted to the DTM service — the tenant admin's
    ...                one-off step (the device never calls the DTM service). An existing
    ...                definition is reconciled rather than trusted: one left over from an
    ...                earlier connector config (a renamed or removed point) is re-created, so
    ...                the Parameters tab never shows stale properties.
    Ensure DTM Property Definition    ${DEFINITION}
    DTM Property Definitions Should Contain    ${SET}
    DTM Property Definition Should Match    ${DEFINITION}
    # A primitive schema is accepted as a device-parameter definition too.
    Ensure DTM Property Definition    ${LITERAL_DEFINITION}
    DTM Property Definition Should Match    ${LITERAL_DEFINITION}

Child Device Carries The Parameter Set Fragment
    [Documentation]    ot-parameter-state publishes the set as a twin fragment; the c8y mapper
    ...                mirrors it into the child's managed object where the Parameters tab reads it.
    Cumulocity.Device Should Exist    ${CHILD_EXTERNAL_ID}
    ${mo}=    Managed Object Should Have Fragments    ${SET}    timeout=${MEAS_TIMEOUT}
    Dictionary Should Contain Key    ${mo}[${SET}]    temp_u16
    Dictionary Should Contain Key    ${mo}[${SET}]    coil_rw

Parameter Update Operation Writes The Points
    [Documentation]    The operation the Parameters tab sends is executed as one connector
    ...                write-batch and completes SUCCESSFUL.
    Cumulocity.Device Should Exist    ${CHILD_EXTERNAL_ID}
    ${operation}=    Cumulocity.Create Operation
    ...    fragments={"c8y_ParameterUpdate":{},"c8y_ParameterUpdate_${SET}":{},"${SET}":{"temp_u16":${NEW_VALUE},"coil_rw":true}}
    ...    description=Update ${SET}
    Cumulocity.Operation Should Be SUCCESSFUL    ${operation}    timeout=${OP_TIMEOUT}

Parameter Fragment Reflects The Update
    Cumulocity.Device Should Exist    ${CHILD_EXTERNAL_ID}
    Cumulocity.Managed Object Should Have Fragment Values    ${SET}.temp_u16\=${NEW_VALUE}    ${SET}.coil_rw\=true    timeout=${MEAS_TIMEOUT}

Measurements Confirm The Written Value
    Cumulocity.Device Should Exist    ${CHILD_EXTERNAL_ID}
    ${measurements}=    Cumulocity.Device Should Have Measurements
    ...    minimum=1    type=modbus    value=modbus    series=temp_u16
    ...    sort_newest=${True}    timeout=${MEAS_TIMEOUT}
    Should Be Equal As Integers    ${measurements[0]["modbus"]["temp_u16"]["value"]}    ${NEW_VALUE}

A Mapped Parameter Shows Its Label And Writes The Code
    [Documentation]    The fragment carries the mapped point's label, and an edit of the label
    ...                from the Parameters tab reaches the register as its code (§4.3): the
    ...                fragment and the next measurement of the same register both follow.
    Cumulocity.Device Should Exist    ${CHILD_EXTERNAL_ID}
    Cumulocity.Managed Object Should Have Fragment Values    ${SET}.state\=running    timeout=${MEAS_TIMEOUT}
    ${operation}=    Cumulocity.Create Operation
    ...    fragments={"c8y_ParameterUpdate":{},"c8y_ParameterUpdate_${SET}":{},"${SET}":{"state":"idle"}}
    ...    description=Update ${SET} by label
    Cumulocity.Operation Should Be SUCCESSFUL    ${operation}    timeout=${OP_TIMEOUT}
    Cumulocity.Managed Object Should Have Fragment Values    ${SET}.state\=idle    ${SET}.temp_u16\=17001    timeout=${MEAS_TIMEOUT}

Child Device Carries The Literal Parameter As A Bare Value
    [Documentation]    The c8y mapper mirrors the literal's twin fragment into the managed object
    ...                as the value itself: `"plc_sim_pump_enabled": false`, not an object.
    Cumulocity.Device Should Exist    ${CHILD_EXTERNAL_ID}
    ${mo}=    Managed Object Should Have Fragments    ${LITERAL}    timeout=${MEAS_TIMEOUT}
    Should Be Equal    ${mo}[${LITERAL}]    ${False}

Literal Parameter Update Writes The Point
    [Documentation]    An edit of a literal parameter carries the bare value. It is executed as
    ...                a write-batch of one write, and the managed object follows.
    Cumulocity.Device Should Exist    ${CHILD_EXTERNAL_ID}
    ${operation}=    Cumulocity.Create Operation
    ...    fragments={"c8y_ParameterUpdate":{},"c8y_ParameterUpdate_${LITERAL}":{},"${LITERAL}":true}
    ...    description=Update ${LITERAL}
    Cumulocity.Operation Should Be SUCCESSFUL    ${operation}    timeout=${OP_TIMEOUT}
    Cumulocity.Managed Object Should Have Fragment Values    ${LITERAL}\=true    timeout=${MEAS_TIMEOUT}

Parameter Update With An Unknown Key Fails
    Cumulocity.Device Should Exist    ${CHILD_EXTERNAL_ID}
    ${operation}=    Cumulocity.Create Operation
    ...    fragments={"c8y_ParameterUpdate":{},"c8y_ParameterUpdate_${SET}":{},"${SET}":{"bogus":1}}
    ...    description=Update ${SET} with an unknown key
    ${op}=    Cumulocity.Operation Should Be FAILED    ${operation}    timeout=${OP_TIMEOUT}
    Should Contain    ${op}[failureReason]    bogus


*** Keywords ***
Setup Child Context
    Setup Cloud Device
    Set Suite Variable    $CHILD_EXTERNAL_ID    ${DEVICE_ID}:device:${CHILD_NAME}
    Cumulocity.Set Device    ${DEVICE_ID}
