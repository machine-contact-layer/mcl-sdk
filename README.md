# MCL SDK

Developer-facing low-level reference SDK for the **Machine Contact Layer**.

The primary reference SDK is a portable C99 implementation designed to run from resource-constrained microcontrollers through embedded systems and hosted applications without changing the protocol contract.

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
 transport callback (raw Wire bytes)
  AP / IP / BLE / UWB / future
```

### Transport Callback Boundary

The SDK defines a minimal byte-transmission callback:

```c
typedef int32_t (*mcl_sdk_tx_fn)(
    void *user,
    const uint8_t *data,
    size_t data_size);
```

> [!IMPORTANT]
> The bytes passed to `mcl_sdk_tx_fn` are **canonical Wire-encoded bytes**, not a normative MCL Link binary frame. The SDK does not prepend fake Link headers or framing wrappers. Physical framing, preamble, modulation, or packet encapsulation remains the responsibility of the underlying transport binding (e.g. MCL-AP, MCL-BLE, MCL-IP, MCL-UWB).

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
    MCL_SDK_ERR_TX_FAILURE = 6
};
```

### Node Model

```c
typedef struct {
    uint16_t supported_wire_majors_mask;
    mcl_sdk_tx_fn tx_fn;   /* Optional: NULL for receive-only nodes */
    void *user_ctx;        /* Passed to tx_fn */
} mcl_node_config_t;

typedef struct {
    mcl_link_t link;
    mcl_sdk_tx_fn tx_fn;
    void *user_ctx;
    uint16_t supported_wire_majors_mask;
} mcl_node_t;
```

### Operations

- `mcl_node_init`: Initializes caller-owned node and underlying Link state machine.
- `mcl_node_reset`: Resets node and transitions Link back to `IDLE`, invalidating any active context.
- `mcl_node_send_tier0`: Encodes a Tier-0 object using `mcl_wire_tier0_encode` into caller-provided scratch and delivers exact Wire bytes to `tx_fn`.
- `mcl_node_receive_tier0`: Decodes raw Wire bytes into a caller-owned `mcl_wire_tier0_t`.
- `mcl_node_link_transition`: Steps the Link state machine through valid lifecycle transitions.
- `mcl_node_link_install_context`: Installs a negotiated context (only permitted in active negotiating/session states).
- `mcl_node_link_authorize_context`: Verifies an incoming context key against the active installed session context.

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

Freestanding C99 reference vertical slice implemented and validated against canonical Wire and Link components.

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
