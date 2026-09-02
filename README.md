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

See [`examples/hello_world.c`](file:///C:/Users/marsm/Downloads/mcl/mcl-sdk/examples/hello_world.c) for a complete two-node in-memory exchange demonstrating:

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
- a session reference may only be emitted once a context is actually installed;
- a malformed or truncated frame is rejected before any semantic decoding, so
  it never reaches the Wire decoder;
- a frame class that carries no semantics yields no object rather than having
  meaning manufactured for it;
- receiving a frame changes no link state and installs no context. An
  `AUTHORITY_CLAIM` arriving in a frame does not become authority by being
  received; it is information for local policy.
