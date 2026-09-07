# MCL SDK

Developer-facing low-level reference SDK for the **Machine Contact Layer**.

The primary reference SDK is a portable C99 implementation designed to run from resource-constrained microcontrollers through embedded systems and hosted applications without changing the protocol contract.

**New here? [`QUICKSTART.md`](QUICKSTART.md)** takes you from a clone to two
machines in contact in eight steps, through the integration facade in
[`include/mcl/machine.h`](include/mcl/machine.h): five platform operations, one
event, and MCL keeps the protocol choreography.

**Integrating MCL into a product?** [`BUILDER_GUIDE.md`](BUILDER_GUIDE.md) walks
the ten questions a builder actually asks and is explicit about what MCL does
not do yet — including that `MCL Stranger-Contact 1` is claimable only with a
stated caveat, and that there is no cryptography anywhere in v1.

## Implementation Contract

The reference stack targets a conservative **C99** subset:

- **Zero dynamic allocation**: `malloc`, `calloc`, `realloc`, and `free` are forbidden in protocol-facing code.
- **Caller-owned memory**: All nodes, state structures, and scratch buffers are owned by the caller.
- **Freestanding portability**: Requires zero OS syscalls, threads, filesystem operations, sockets, or runtime libc symbols.
- **Deterministic error handling**: Returns explicit `mcl_sdk_status_t` codes rather than relying on global error states.
- **No protocol mutation**: The SDK does not redefine Wire encoding, does not alter Link lifecycle semantics, and does not maintain a private semantic registry.
- **Policy sovereignty**: Receiving an `AUTHORITY_CLAIM` or `REQUEST` produces decoded objects and nothing more. The SDK performs no automated authorization, privilege escalation, or policy decisions.

## Layer Relationship

```text
application / product policy
          |
       MCL SDK
          |
  +-------+-------+
  |               |
MCL Core        MCL Link
  |               |
  +---- MCL Wire--+
          |
 transport callback (told WHICH bearer)
  AP / IP / BLE / UWB / future
```

### Transport Callback Boundary

```c
typedef int32_t (*mcl_sdk_tx_fn)(
    void *user,
    uint8_t transport_id,
    const uint8_t *data,
    size_t data_size);
```

**The transport is a parameter, and that is what makes migration
expressible.** An earlier revision had no `transport_id`, so a node had exactly
one way out — which cannot express a migration, the one thing this layer exists
to do. During a migration a contact spans two media at once:

```text
TRANSPORT_OFFER / ACCEPT        old transport
PATH_CHALLENGE / PATH_RESPONSE  candidate transport
COMMIT / CONFIRM                candidate transport
ordinary traffic                depends on the cutover state
```

With one untagged callback an integrator had to infer which socket,
characteristic or speaker each call meant from the order of calls. The SDK now
derives it from the contact state and says so on every call.

**The return value is three-way, and the distinction is load-bearing:**

| Return | Meaning |
|---|---|
| `0` | Accepted for transmission. |
| `< 0` | **Definitely** not transmitted. Nothing left this machine. |
| `> 0` | Outcome **unknown**. It may or may not have left. |

This exists because of `COMMIT`. Committing a migration is irrevocable once the
bytes are transmitted, so the SDK must not enter that state for a frame the
transport is certain it never sent — and *must* enter it for one the transport
cannot vouch for, because the peer may have it. Collapsing "not sent" and
"unknown" forces a choice between a contact stuck irrevocably on nothing and a
rollback after a commit the peer may have acted on. A transport that genuinely
cannot tell the difference **MUST** return `> 0`.

> [!IMPORTANT]
> What the callback carries depends on which path produced it. The raw Tier-0
> path (`mcl_node_send_tier0`) delivers canonical Wire bytes, which is what a
> bearer like MCL-AP carries directly. The framed and handoff paths deliver
> complete MCL Link frames, which is what the IP, BLE and UWB bindings carry.
> Physical framing, preamble, modulation and packet encapsulation remain the
> transport binding's responsibility in both cases.

## Public API Overview

### Types and Status Codes

```c
typedef int32_t mcl_sdk_status_t;
enum {
    MCL_SDK_OK = 0,
    MCL_SDK_ERR_INVALID_ARGUMENT = 1,
    MCL_SDK_ERR_BUFFER_TOO_SMALL = 2,
    MCL_SDK_ERR_WIRE_FAILURE = 3,
    MCL_SDK_ERR_LINK_FAILURE = 4,
    MCL_SDK_ERR_TX_UNAVAILABLE = 5,
    MCL_SDK_ERR_TX_FAILURE = 6,
    MCL_SDK_ERR_FRAME_FAILURE = 7,
    MCL_SDK_ERR_INVALID_STATE = 8,
    MCL_SDK_ERR_TX_NOT_SENT = 9,    /* transport is certain nothing left */
    MCL_SDK_ERR_TX_UNCERTAIN = 10,  /* transport cannot say; retransmit */
    MCL_SDK_ERR_WRONG_TRANSPORT = 11,
    MCL_SDK_ERR_QUIESCED = 12,      /* cutover in progress; not an error */
    MCL_SDK_NOT_ADDRESSED = 13      /* decoded, addressed elsewhere */
};
```

### Node Model

```c
typedef struct {
    uint16_t supported_wire_majors_mask;
    mcl_sdk_tx_fn tx_fn;   /* Optional: NULL for receive-only nodes */
    void *user_ctx;        /* Passed to tx_fn */
    uint32_t source_ref;   /* Contact reference, NOT an identity */
    uint8_t transport_id;  /* Which bearer this contact begins on */
    mcl_contact_role_t role;  /* Ordering only; confers no authority */
} mcl_node_config_t;

typedef struct {
    mcl_link_t link;       /* protocol lifecycle */
    mcl_contact_t contact; /* transport continuity */
    mcl_sdk_tx_fn tx_fn;
    void *user_ctx;
    uint16_t supported_wire_majors_mask;
    uint32_t source_ref;
    uint16_t tx_sequence;
} mcl_node_t;
```

A node holds **two** state machines answering different questions. `mcl_link_t`
is the protocol lifecycle: have we discovered a peer, exchanged capabilities,
negotiated, established? `mcl_contact_t` is transport continuity: which medium
carries this contact, and is a change of medium under way? Neither implies the
other and neither is derived from the other. They cross in exactly one place,
which the SDK owns: a migration may only be driven while the lifecycle is
`ESTABLISHED` or `HANDOFF`, and the lifecycle may not leave those states while a
migration is outstanding.

One node tracks one contact. A machine holding several concurrent contacts
instantiates several nodes; that is a real limitation rather than an oversight,
recorded in `mcl-link/research/secure-contact-threat-model.md`.

### Operations

**Node lifecycle**
- `mcl_node_init`, `mcl_node_reset`

**Raw Tier-0 path** — canonical Wire bytes, for bearers that carry them directly
- `mcl_node_send_tier0`, `mcl_node_receive_tier0`

**Framed contact path** — MCL Link frames
- `mcl_node_send_framed_tier0`, `mcl_node_receive_framed`

**Extension-aware framed path** — for objects carrying Wire extensions
- `mcl_node_send_framed_tier0_ext`, `mcl_node_receive_framed_ext`

**Handoff control path** — migration as an on-wire protocol
- `mcl_node_send_handoff`, `mcl_node_receive_handoff`, `mcl_node_apply_handoff`

**State access**
- `mcl_node_get_contact`, `mcl_node_get_contact_const`
- `mcl_node_get_link`, `mcl_node_get_link_const`
- `mcl_node_link_transition`, `mcl_node_link_install_context`,
  `mcl_node_link_authorize_context`

Receiving is transport-aware throughout: `mcl_node_receive_framed` and
`mcl_node_receive_handoff` take the arrival transport and refuse a frame that
arrived on a bearer this contact does not live on. Without that argument the SDK
could not perform the one check path validation depends on — a `PATH_RESPONSE`
fed in from the old path would otherwise validate a candidate that had never
carried a single byte.

Addressing is explicit: `MCL_LINK_FLAG_DESTINATION` addresses a frame to this
contact's peer, whose reference was learned during first contact. There is no
destination parameter, because a node holds one contact and can address only
that contact's peer.

## Minimal Example

See [`examples/hello_world.c`](examples/hello_world.c) for a complete two-node in-memory exchange demonstrating:

```text
Node A (Presence semantic)
  -> mcl_node_send_tier0()
  -> Wire encoding
  -> in-memory transport callback
  -> Node B
  -> mcl_node_receive_tier0()
  -> decoded semantic object
```

## Status

Freestanding C99 reference implementation, validated against canonical Wire and
Link components and exercised between two machines over real radios.

For what this repository claims in the v1.0 release, see
[`mcl-core/governance/V1_SCOPE.md`](../mcl-core/governance/V1_SCOPE.md). The
public API is Stable in v1 at **source compatibility only**; no binary ABI
stability is promised.

## Framed contact path

`mcl_node_send_tier0` and `mcl_node_receive_tier0` move raw canonical Wire
bytes, which is what a bearer like MCL-AP carries directly.

`mcl_node_send_framed_tier0` and `mcl_node_receive_framed` move MCL Link frames,
which is what the IP, BLE and UWB bindings carry and what a session needs: a
frame class, a session reference, a sequence, and optional integrity.

Both exist deliberately. A minimal broadcast beacon has no use for a session
reference, and forcing one would spend bytes on exactly the transport where
bytes are scarcest.

Properties the tests pin:

- a transmit sequence advances only after the transport accepted the frame, so
  a failed send leaves no gap;
- a session reference comes from the **contact**, never from the Wire context.
  An earlier revision emitted `active_context.context_id` as the frame's
  `session_ref`, and a test asserted that behaviour, which is how it survived. A
  machine can hold a session with no context, one session across several context
  generations, or one contact with an entirely new context; deriving either from
  the other made all three unrepresentable. Emitting a session reference before
  a migration has been agreed is refused rather than filled with a placeholder;
- a malformed or truncated frame is rejected before any semantic decoding, so
  it never reaches the Wire decoder;
- a frame class that carries no semantics yields no object rather than having
  meaning manufactured for it;
- receiving a frame changes no link state and installs no context. An
  `AUTHORITY_CLAIM` arriving in a frame does not become authority by being
  received; it is information for local policy.

## Handoff control path

`mcl_node_send_handoff`, `mcl_node_receive_handoff` and `mcl_node_apply_handoff`
carry the migration controls defined in
[`mcl-link/spec/link-handoff-control-v0.1.md`](../mcl-link/spec/link-handoff-control-v0.1.md)
as the payload of a `HANDOFF` Link frame.

This is what makes migration an on-wire protocol rather than a sequence of local
calls. `TRANSPORT_OFFER` and `TRANSPORT_ACCEPT` have had canonical bytes since
Wire v0.3; `PATH_CHALLENGE`, `PATH_RESPONSE`, `COMMIT` and `CONFIRM` did not,
which meant two implementations written from the specification could agree on
the offer and then exchange nothing further. **A hardware run driven by direct
calls to `mcl_contact_*` on both machines proves the radios work, not that the
migration is specified**, and must never be recorded as on-wire migration.

Sending, receiving and applying are three steps on purpose. Receiving decodes
and changes nothing; applying is an explicit call the caller makes once its own
policy has decided to, because the interaction sequence belongs to the
deployment (charter 2.10.1) and reception must never become authority
(charter 2.3).

`mcl_node_apply_handoff` drives the existing `mcl_contact_*` API and reports
what the caller should send back. It contains no second state machine — a
duplicate would drift from the first, and the two would disagree exactly when a
migration was already going wrong. The library does not transmit the reply
itself: during a migration the candidate and the current transport are
different, and only the caller knows which one the reply belongs on.

`tests/test_sdk_handoff.c` runs a complete migration between two nodes in which
**the only thing crossing between them is a byte buffer**, including the case
where a `CONFIRM` is dropped after a successful send and the peers recover by
retransmitting `COMMIT`.

Completing the sequence establishes reachability on the candidate path and
nothing else. Every reference in it crosses an observable medium in the clear.

## Migration over real radios

`tests/test_sdk_handoff.c` proves the sequence between two nodes sharing a
buffer. That is not the same as proving it over a medium, so it was also run
over two.

[`hardware/esp32-dual-peer`](hardware/esp32-dual-peer) holds a Wi-Fi SoftAP with
a UDP socket **and** a BLE GATT server up simultaneously on one ESP32-S3, with a
single `mcl_node_t` across both;
[`tools/dual-transport-peer`](tools/dual-transport-peer) is the host half. One
contact survived **104 changes of medium**, including 100 consecutive
alternating BLE↔IP migrations, across 2989 checks with none failed, and the host
and board records agree frame for frame.

The case that needed two live radios is a control that is correct in every
reference delivered over the **wrong** medium. It was refused. That is the check
path validation depends on, and a single-transport rig cannot construct it at
all.

The run also found a defect no loopback test could: every send path honoured
`MCL_LINK_FLAG_DESTINATION` and then set `destination_ref` to zero, so an
addressed frame was addressed to nobody. See
[`evidence/e4-dual-transport-migration-20260903`](evidence/e4-dual-transport-migration-20260903).

None of this is independent interoperability. Both ends compile these same
sources, so a shared misreading of the specification passes on both sides and is
invisible in the result.
