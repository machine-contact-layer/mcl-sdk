# MCL SDK

Developer-facing reference SDK for the **Machine Contact Layer**.

The SDK is intentionally transport-neutral. Applications interact with MCL semantic objects and events while transport bindings implement MCL-AP, IP, BLE, UWB, or future profiles underneath.

## Target developer experience

```python
from mcl import Node, Hazard, Priority

node = Node()

@node.on(Hazard)
async def on_hazard(event: Hazard):
    print(event)

await node.start()

await node.broadcast(
    Hazard(
        hazard_class="collision_risk",
        severity=3,
        confidence=0.94,
        priority=Priority.CRITICAL,
    )
)
```

## Design rules

- no AI model is required to use the protocol
- semantic objects are explicit and typed
- transports are pluggable
- local policy decides how received claims/requests affect machine behavior
- transport-specific metrics are available below the high-level API
- the SDK should remain small enough for embedded/reference implementations to reproduce the core behavior

## Initial package

The pre-v0.1 package includes:

- semantic event dataclasses
- priority classes
- async event handlers
- pluggable transport interface
- in-memory transport for tests and examples

Acoustic, BLE, UWB, and IP transports will plug into the same interface as their repositories mature.

## Status

Private research repository. Pre-v0.1 reference implementation. API is not stable.
