# Porting MCL to a machine

The normal integration boundary is one header, `mcl/machine.h`, and one
versioned platform structure, `mcl_platform_v1_t`. Application code handles
machine events. Platform code implements hardware and operating-system actions.
Neither constructs Wire objects nor drives Link or rendezvous state.

## Required operations for MCL-REFERENCE-DEPLOYMENT-1

| operation | contract |
|---|---|
| `clock_ms` | Monotonic 32-bit milliseconds. Wrap is supported. |
| `random_bytes` | Fill the requested bytes and return 0. Required for shared-medium contention and non-zero transaction references. |
| `transport_send` | Return 0 if sent, negative if definitely not sent, positive if the outcome is unknown. Never collapse refusal and uncertainty. |
| `medium_busy` | Non-zero while another shared-medium transmission is in progress or being acquired. |
| `self_transmitting` | Non-zero only while this machine's own emitter is active. Do not infer this from `source_ref`. |
| `candidate_open` | Begin opening the agreed continuation bearer and return READY, PENDING, or REFUSED. |

`candidate_close` and `policy_admit` are optional. Without the latter, the
application receives `MCL_MACHINE_EVENT_POLICY_REQUIRED` and answers explicitly
with `mcl_machine_admit()` or `mcl_machine_refuse()`.

## Scheduling and asynchronous work

Call `mcl_machine_poll()` from a regular timer and promptly after delivering
receive, candidate-ready, or policy input. Late calls directly delay scheduled
transmission and timeout handling. The reference host and embedded adapters use
a 10 ms service cadence.

`candidate_open` must not block while a BLE scan, connection, or socket setup
runs. Return `MCL_MACHINE_CANDIDATE_PENDING`, perform the work asynchronously,
then call `mcl_machine_candidate_ready()` once the bearer can carry bytes or
`mcl_machine_candidate_refused()` if opening later fails.

Incoming bytes from every bearer go to `mcl_machine_receive()`. The transport
adapter retains ownership of its receive buffer after the call returns.

## Ownership and concurrency

MCL allocates no memory. The application owns `mcl_machine_t` and must keep it
alive for the contact lifetime. Calls for one instance are serialized by the
adapter; the v1 facade does not add locks or enter callbacks concurrently.

Callbacks may be invoked from `mcl_machine_start()`, `mcl_machine_poll()`,
`mcl_machine_receive()`, and the explicit candidate/policy answer functions.
They must not call back into the same `mcl_machine_t` recursively.

## What the application sees

The application consumes these outcomes:

- `PEER_DETECTED`: correlation only; never identity.
- `POLICY_REQUIRED`: local code must admit or refuse.
- `CONTACT_ESTABLISHED`: reachability and correlation on the selected bearer.
- `CONTACT_LOST`: the bounded contact attempt or established contact ended.
- `NO_COMMON_BEARER`: negotiation completed without a deployable intersection.
- `ERROR`: the platform or facade could not honour its contract.

`CONTACT_ESTABLISHED` does not mean authenticated or authorized. MCL v1 ships
no cryptography.

## Port acceptance checklist

A platform port is not complete until it proves:

- no peer address, endpoint token, session reference, secret, or pairing state
  is compiled into the application;
- definite transmit refusal and uncertain transmit outcome are distinguishable;
- a pending candidate never carries MCL bytes before `candidate_ready`;
- candidate resources close after refusal, timeout, or contact loss;
- shared-medium sensing observes both other traffic and self transmission;
- the documented service cadence is sustained under positive traffic, not only
  in an idle room;
- the same application source runs with the reference host adapter and the new
  platform adapter.

The DFR1154 implementation under `hardware/` is the embedded reference. Its
platform-specific audio, BLE, Wi-Fi, memory, and scheduling code is not part of
the application contract.
