*** Settings ***
Documentation       Interoperability with a third-party OPC UA stack: the connector against the
...                 OPC Foundation UA-.NETStandard reference server (php-opcua/uanetstandard-
...                 test-suite, MIT). Everything else we test against is a server this project
...                 wrote, so a misreading of the specification is invisible to it; this suite
...                 exists to catch that.
...
...                 One device per situation (connectors/opcua/interop/connector.toml.in, whose
...                 namespace index is resolved at startup by resolve-ns.py).
...
...                 Run via:  just test-interop opcua   (and just test-interop-c opcua)

Resource            ../../../_shared/stack.resource
Library             Collections
Library             String

Suite Setup         Setup Interop Stack
Suite Teardown      Teardown OT Stack


*** Variables ***
${PROTOCOL}             opcua
${CONFIG}               /rendered/connector.toml
${READY_TIMEOUT}        150
${SAMPLE_TIMEOUT}       30
# A failing device retries on a 1s -> 60s backoff.
${RECONNECT_TIMEOUT}    150
# TestServer/DataTypes/Scalar/DoubleValue, a constant of the reference address space.
${DOUBLE}               2.71828


*** Test Cases ***
Every Supported Policy And Mode Connects
    [Documentation]    Each policy the connector supports, negotiated with a real server, and
    ...                the effective policy and mode reported back in the link status.
    ...                Aes128_Sha256_RsaOaep had never been exercised positively before: our own
    ...                simulators only ever offered it to produce a "no matching endpoint"
    ...                reason.
    [Template]    Device Connects With
    b256-sign     Basic256Sha256           sign
    b256-seal     Basic256Sha256           sign_and_encrypt
    aes128        Aes128_Sha256_RsaOaep    sign_and_encrypt
    aes256        Aes256_Sha256_RsaPss     sign_and_encrypt

Unsecured Channel Connects And Reads Every Datatype
    [Documentation]    Policy None, and one point per datatype the connector claims, read from
    ...                the reference address space.
    Wait For Link Status    plain    connected
    Sample Should Be    plain    double    ${DOUBLE}
    Sample Should Be    plain    bool      ${1}
    Sample Should Be    plain    int32     ${-100000}
    Sample String Should Be    plain    string    Hello OPC UA

Structured Values Are Decoded From The Reference Server
    [Documentation]    Fields of the server's own structure types (TestPointXYZ, TestRangeStruct),
    ...                found through the values' encodings and decoded from the DataTypeDefinition
    ...                the server publishes; the raw body; and an array element by IndexRange.
    [Tags]    requires:opcua-structures
    Wait For Link Status    structures    connected
    Sample Should Be    structures    point_x    1.5
    Sample Should Be    structures    point_z    3.5
    Sample Should Be    structures    range_high    100
    Sample Should Be    structures    range_value    42.5
    Sample Should Be    structures    double_array_2    3.33
    ${bad}=    Wait For Sample    te/device/structures/ot/${PROTOCOL}/sample/range_max
    ...    timeout=${SAMPLE_TIMEOUT}
    ${error}=    Get Json Field    ${bad}    error
    Should Be Equal    ${error}    field "Max" not found in TestRangeStruct (fields: Low, High, Value)
    ${raw}=    Wait For Sample    te/device/structures/ot/${PROTOCOL}/sample/point_raw
    ...    timeout=${SAMPLE_TIMEOUT}
    ${bytes}=    Get Json Field    ${raw}    raw
    Should Be Equal    ${bytes}    00 00 00 00 00 00 f8 3f 00 00 00 00 00 00 04 40 00 00 00 00 00 00 0c 40
    ${encoding}=    Get Json Field    ${raw}    addr.encoding_id
    Should Start With    ${encoding}    nsu=urn:opcua:test-server:custom-types;

Deprecated Basic256 Connects With The Opt-In
    [Documentation]    Basic256 had only ever been covered by configuration-validation unit
    ...                tests; here it runs against a server that really offers it. The refusal
    ...                WITHOUT `allow_deprecated_security` stays a configuration concern,
    ...                covered by connector-opcua's own unit tests -- it never reaches a
    ...                server, so it does not belong in an interop suite.
    Device Connects With    legacy-b256    Basic256    sign_and_encrypt

Deprecated Basic128Rsa15 Connects With The Opt-In
    [Documentation]    The C build cannot complete this handshake against UA-.NETStandard:
    ...                open62541 aborts with BadDecodingError while async-opcua connects to the
    ...                same endpoint, so the fault is in the policy implementation, not the
    ...                server or the configuration. Basic128Rsa15 is deprecated and open62541
    ...                itself warns that its encryption is broken, so this is recorded as a
    ...                parity gap rather than worked around.
    [Tags]    requires:opcua-basic128rsa15
    Device Connects With    legacy-b128    Basic128Rsa15    sign_and_encrypt

Username Identity Is Accepted And Rejected As The Server Decides
    [Documentation]    The reference server's own user database (config/users.json). The
    ...                rejection must name the identity, never the password.
    Wait For Link Status    user    connected
    Sample Should Be    user    double    ${DOUBLE}
    ${reason}=    Link Is Refused    baduser    identity rejected:
    Should Not Contain    ${reason}    not-the-password
    ${logs}=    Connector Log
    Should Not Contain    ${logs}    not-the-password

Weak Server Certificate Is Refused With Its Key Length
    [Documentation]    UA-.NETStandard auto-generates its application certificate with a
    ...                1024-bit RSA key (upstream says so in Program.cs), which Basic256Sha256
    ...                forbids. ref-discovery keeps that certificate on purpose.
    ...
    ...                The key length belongs to the security policy, not to trust, so this
    ...                device sets `trust_any_server_certificate` and must STILL be refused --
    ...                that option says "do not judge who this server is", not "use a key the
    ...                policy forbids". The reason must name the key length, and must not
    ...                advise `tedge-dot pki trust`: trusting a certificate cannot lengthen its
    ...                key.
    ${reason}=    Link Is Refused    weakcert    certificate invalid:
    Should Contain    ${reason}    1024
    Should Contain    ${reason}    Basic256Sha256
    Should Not Contain    ${reason}    tedge-dot pki trust

Untrusted Server Is Quarantined Until Trusted
    [Documentation]    The reference server regenerates its certificate on every start, so
    ...                nothing can be pinned ahead of time: this is the operator flow
    ...                (quarantine, inspect, `tedge-dot pki trust`) against a real server, and
    ...                the trust must take effect without restarting the connector.
    ${reason}=    Link Is Refused    tofu    certificate untrusted:
    ${thumbprint}=    Thumbprint In Reason    ${reason}
    ${rejected}=    Pki    list rejected --json
    Should Contain    ${rejected}    ${thumbprint}
    Pki    trust ${thumbprint}
    ${link}=    Wait For Link Status    tofu    connected
    Should Be Equal    ${{ json.loads($link)["info"]["server_certificate"] }}    trusted
    Should Be Equal    ${{ json.loads($link)["info"]["server_thumbprint"] }}    ${thumbprint}

Server That Refuses Our Certificate Says So
    [Documentation]    ref-strict does not auto-accept an unknown client certificate, and we
    ...                trust it (trust_any), so the only thing that can fail is its judgement of
    ...                us. Both directions of distrust arrive as the same status code, so this
    ...                is the one case that tells the categories apart on a real server.
    ${reason}=    Link Is Refused    strict    application certificate rejected:
    ${own}=    Own Thumbprint
    Should Contain    ${reason}    ${own}
    Should Contain    ${reason}    tedge-dot pki export
    # Our own distrust of a server must NOT be reported this way.
    Should Not Start With    ${reason}    certificate untrusted:

Server Trusts Us After Our Certificate Is Added
    [Documentation]    entrypoint.sh exported our certificate into the volume ref-strict mounts
    ...                at /app/certs; that server reads its trust list only at startup, so the
    ...                trust takes effect on restart.
    [Setup]    Link Is Refused    strict    application certificate rejected:
    Restart Stack Service    ref-strict
    Wait For Link Status    strict    connected

Policies We Do Not Implement Are Listed
    [Documentation]    A server offering only elliptic-curve policies must produce a listed
    ...                refusal naming them as the server advertised them, not an empty list, and
    ...                must not take the connector down with it.
    ${reason}=    Link Is Refused    ecc    no matching endpoint:
    Should Contain    ${reason}    ECC_nistP256
    Should Contain    ${reason}    ECC_nistP384
    # The other devices keep working while that one fails.
    Sample Should Be    plain    double    ${DOUBLE}

Server Advertising 0.0.0.0 Is Reachable
    [Documentation]    The reference server advertises `opc.tcp://0.0.0.0:4840/...` as its
    ...                endpoint URL (BuildBaseAddresses hardcodes it). A client that dials what
    ...                the server advertises cannot connect at all -- asyncua gets
    ...                BadServerUriInvalid -- so this is the real-world case for keeping the
    ...                configured address.
    ${link}=    Wait For Link Status    plain    connected
    Should Be Equal    ${{ json.loads($link)["info"]["endpoint"] }}
    ...    opc.tcp://ref-all:4840/UA/TestServer

Endpoint Without A Resource Path Connects
    [Documentation]    The discovery instance serves an endpoint URL with no resource path.
    Wait For Link Status    discovery    connected
    Sample Should Be    discovery    double    ${DOUBLE}

Subscription Delivers Changes From The Reference Server
    [Documentation]    TestServer/Dynamic/Counter is updated by the server on a timer, so the
    ...                push path can be told apart from polling: two different values must
    ...                arrive without that point being polled.
    Wait For Link Status    dynamic    connected
    ${first}=    Wait For Sample    te/device/dynamic/ot/${PROTOCOL}/sample/counter
    ...    timeout=${SAMPLE_TIMEOUT}
    ${second}=    Wait For Fresh Sample Differing From    dynamic    counter    ${first}
    Should Not Be Equal    ${first}    ${second}

Session Survives Secure Channel Token Renewal
    [Documentation]    ref-token grants a 30s secure channel token, so staying connected across
    ...                this test means the channel was renewed rather than re-established.
    [Timeout]    5 minutes
    Wait For Link Status    token    connected
    Sleep    75s    reason=two 30s token lifetimes, so at least two renewals fall inside
    Sample Should Arrive    token    counter
    # The link status is published on change, so its whole history over the window is the
    # evidence: a renewal that failed would have dropped the session and published
    # `disconnected` before reconnecting. Asserted on the published status rather than on log
    # text, which the two runtimes word differently ("device token: link disconnected" in C,
    # "link status changed device=token status=disconnected" in Rust) -- a log-string check
    # silently matches neither and can never fail.
    ${history}=    Get Messages    te/device/token/ot/${PROTOCOL}/status/link
    ${states}=    Evaluate    [json.loads(m)["status"] for m in $history]    modules=json
    Should Not Contain    ${states}    disconnected
    ...    msg=the session dropped and reconnected instead of renewing its token: ${states}
    Should Contain    ${states}    connected


*** Keywords ***
Setup Interop Stack
    Setup OT Stack    opcua    compose_file=${CURDIR}/../docker-compose.yaml

Pki
    [Documentation]    Run `tedge-dot pki <args>` in the connector container; return stdout.
    [Arguments]    ${args}
    ${out}=    DeviceLibrary.Execute Command
    ...    cmd=tedge-dot pki ${args} --config ${CONFIG}    strip=${True}
    RETURN    ${out}

Thumbprint In Reason
    [Documentation]    The SHA-1 thumbprint the link reason names. Several servers can sit in
    ...                `rejected/` at once and the listing order is not a contract, so the
    ...                reason decides which certificate this test is about.
    [Arguments]    ${reason}
    ${matches}=    Get Regexp Matches    ${reason}    thumbprint ([0-9a-f]{40})    1
    Should Not Be Empty    ${matches}    msg=no thumbprint in reason: ${reason}
    RETURN    ${matches}[0]

Own Thumbprint
    [Documentation]    The connector's own application certificate thumbprint.
    ${out}=    Pki    show --json
    RETURN    ${{ json.loads($out)["thumbprint"] }}

Connector Log
    [Documentation]    The connector container's log so far.
    ${project}=    DeviceLibrary.Get Compose Project Name
    ${out}=    Run    docker compose -p ${project} logs connector 2>&1
    RETURN    ${out}

Wait For Link Status
    [Documentation]    Wait for the device's retained link status to be `status`; return it.
    [Arguments]    ${device}    ${status}
    ${topic}=    Set Variable    te/device/${device}/ot/${PROTOCOL}/status/link
    ${payload}=    Wait For Fresh Message With Field    ${topic}    status    ${status}
    ...    timeout=${READY_TIMEOUT}    since=0
    RETURN    ${payload}

Link Is Refused
    [Documentation]    The device is disconnected with a reason starting with `prefix`;
    ...                return the reason.
    [Arguments]    ${device}    ${prefix}
    ${link}=    Wait For Link Status    ${device}    disconnected
    ${reason}=    Get Json Field    ${link}    reason
    Should Start With    ${reason}    ${prefix}
    RETURN    ${reason}

Device Connects With
    [Documentation]    The device is connected and reports the effective policy and mode.
    [Arguments]    ${device}    ${policy}    ${mode}
    ${link}=    Wait For Link Status    ${device}    connected
    Should Be Equal    ${{ json.loads($link)["info"]["security_policy"] }}    ${policy}
    Should Be Equal    ${{ json.loads($link)["info"]["security_mode"] }}    ${mode}
    Sample Should Be    ${device}    double    ${DOUBLE}

Sample Should Be
    [Documentation]    The device's next sample of `point` carries `expected`, compared as a
    ...                number. Robot variables are strings, and the published value is JSON, so
    ...                the comparison has to name the type rather than guess it.
    [Arguments]    ${device}    ${point}    ${expected}
    ${value}=    Sample Value    ${device}    ${point}
    Should Be Equal As Numbers    ${value}    ${expected}

Sample String Should Be
    [Arguments]    ${device}    ${point}    ${expected}
    ${value}=    Sample Value    ${device}    ${point}
    Should Be Equal As Strings    ${value}    ${expected}

Sample Value
    [Arguments]    ${device}    ${point}
    ${sample}=    Wait For Sample    te/device/${device}/ot/${PROTOCOL}/sample/${point}
    ...    timeout=${SAMPLE_TIMEOUT}
    ${value}=    Get Json Field    ${sample}    value
    RETURN    ${value}

Sample Should Arrive
    [Arguments]    ${device}    ${point}
    Wait For Sample    te/device/${device}/ot/${PROTOCOL}/sample/${point}
    ...    timeout=${SAMPLE_TIMEOUT}

Wait For Fresh Sample Differing From
    [Documentation]    Wait until the point publishes a value different from `previous`.
    [Arguments]    ${device}    ${point}    ${previous}
    ${topic}=    Set Variable    te/device/${device}/ot/${PROTOCOL}/sample/${point}
    FOR    ${i}    IN RANGE    30
        ${sample}=    Wait For Sample    ${topic}    timeout=${SAMPLE_TIMEOUT}
        # Compared with a keyword, not an IF expression: the payloads are JSON, and Robot
        # interpolates them into the expression before evaluating it.
        ${same}=    Run Keyword And Return Status    Should Be Equal    ${sample}    ${previous}
        IF    not ${same}    RETURN    ${sample}
    END
    Fail    ${device}/${point} never changed: the subscription delivered nothing new

