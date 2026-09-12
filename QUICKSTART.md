# Quickstart

From nothing to two machines in contact. No prior MCL knowledge assumed, and no
architecture charter required to get through it.

If you want the reasoning behind any of it, everything here links down into the
specifications. If you want the deep integration questions — authentication,
trust roots, capability negotiation, what will change under you — read
[`BUILDER_GUIDE.md`](BUILDER_GUIDE.md) next.

---

## 01 What MCL does

Two machines end up in the same place with no shared network, no common
credential system and nobody to introduce them. MCL gives them something to
speak first, and a way to move to a better bearer once they have met.

```text
they hear each other       ->  PRESENCE, over whatever medium exists
they agree on a bearer     ->  TRANSPORT_OFFER / TRANSPORT_ACCEPT
they prove it reaches      ->  PATH_CHALLENGE / PATH_RESPONSE
each side applies policy   ->  admit this stranger, or do not
the contact moves          ->  COMMIT / CONFIRM
```

Your application gets one event: **a contact is established**.

## 02 What MCL does not do

Read this before you design anything around it.

**There is no cryptography in MCL v1.** No confidentiality, no peer
authentication, no replay protection, no signed capabilities. A completed
contact establishes **reachability** and **correlation** and nothing else:

```text
reception != identity != authenticity != authority != trust != obligation
```

Anything in range can complete the sequence and be accepted exactly as an honest
peer would. If you need to know *who* you are talking to, put MCL inside
something that authenticates — DTLS, BLE Secure Connections, a private network —
and see [`../mcl-core/SECURITY.md`](../mcl-core/SECURITY.md).

MCL is also not an autonomy stack, a fleet manager, a credential authority or a
modem, and it never touches your actuators: a peer talks to MCL, not to your
machine.

## 03 Build

The release ships one self-contained developer SDK: one include tree, one
library, one CMake project. Build it directly:

```sh
cmake -S mcl-developer-sdk -B build
cmake --build build
./build/mcl_first_contact
```

Maintainers working from the separately governed repositories generate that
same package, then verify a scratch consumer against it:

```sh
sh mcl-sdk/packaging/make-developer-sdk.sh /tmp/mcl-developer-sdk
sh mcl-sdk/packaging/verify-developer-sdk.sh
```

The multi-repository developer build remains available when changing MCL
itself:

```sh
cmake -S mcl-sdk -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Everything protocol-facing is **freestanding C99** — no heap, no OS, no libc at
runtime, caller-owned memory. It builds for a microcontroller because it was
built on one.

To install the single developer SDK and consume it from your own project:

```sh
cmake -S mcl-developer-sdk -B build -DCMAKE_INSTALL_PREFIX=/your/prefix
cmake --build build && cmake --install build
```

```cmake
find_package(mcl_sdk REQUIRED)
target_link_libraries(my_machine PRIVATE mcl::mcl_sdk)
```

## 04 Run two machines that already share a bearer

```sh
./build/mcl_first_contact
```

```text
MCL contact continuity, MCL-REFERENCE-DEPLOYMENT-1
Two machines using an agreed simulated bearer.

  A  heard a peer, correlation B2B2B2B2
  B  open bearer 3: scanning for the peer's token 7913F31C
  A  open bearer 3: advertising my own token 7913F31C
  B  policy: admit peer A1A1A1A1? yes
  A  policy: admit peer B2B2B2B2? yes
  B  CONTACT ESTABLISHED on transport 3, profile 1, session 6C43B23E
  A  CONTACT ESTABLISHED on transport 3, profile 1, session 6C43B23E
```

**Be clear about what that is.** Both machines are in one process on a simulated
medium and a simulated clock. The *sequence* is real — every frame is the same
canonical Wire and Link byte stream two boards exchange — but nothing physical
is established by it: no acoustic path, no radio, no timing margin, no
interoperability. It is the API and the shape of an integration.

The source is [`examples/first_contact.c`](examples/first_contact.c), and the
part that is yours is 60 lines.

## 05 Optional: observe Stranger-Contact on real hardware

Two machines, two radios, no peer configuration anywhere:

- [`hardware/dfr1154-autonomous-node/`](hardware/dfr1154-autonomous-node/) — an
  ESP32-S3 that owns its own clock, randomness, microphone, speaker and radios,
  and runs the whole sequence with nothing attached. Its README is the wiring
  diagram for a real integration, including the memory campaign that decided
  its shape.
- [`../mcl-ble/hardware/host-ble-probe/`](../mcl-ble/hardware/host-ble-probe/) —
  a laptop with a Bluetooth radio can check what that board puts on the air
  against the profile, sharing no code with it.

## 06 Integrate your own platform

**Eight operations. Six required, two optional.** If porting MCL to a machine
ever needs fifty, the facade is not finished — that is the standard
[`include/mcl/machine.h`](include/mcl/machine.h) is written to.

| operation | required | what it is |
|---|---|---|
| `clock_ms` | yes | monotonic milliseconds; need not be wall time |
| `random_bytes` | on a shared medium | contention backoff and references |
| `transport_send` | yes | put bytes on a medium |
| `medium_busy` | on a shared medium | is anyone transmitting right now |
| `self_transmitting` | on a shared medium | is that me |
| `candidate_open` | yes | make the agreed bearer usable |
| `candidate_close` | no | release it |
| `policy_admit` | no | admit this proposed contact? |

```c
mcl_machine_config_t config;
mcl_platform_v1_t platform = { /* your eight operations */ };
mcl_machine_t machine;
mcl_machine_event_t event;

mcl_machine_config_deployment(&config, MCL_DEPLOYMENT_REFERENCE_1,
                              my_source_ref, MCL_CONTACT_ROLE_INITIATOR);
mcl_machine_init(&machine, &config, &platform);
mcl_machine_start(&machine);

for (;;) {
    /* Service on a regular timer; the reference integrations use 10 ms. */
    mcl_machine_poll(&machine, &event);
    if (event.kind == MCL_MACHINE_EVENT_CONTACT_ESTABLISHED) { /* yours */ }

    /* whenever bytes arrive on any bearer */
    mcl_machine_receive(&machine, transport_id, data, size);
}
```

Three things will bite you if you skim:

**`transport_send` returns three values, not two.** `0` sent, **negative**
definitely not sent, **positive** cannot tell. Do not collapse the last two: MCL
uses the difference to decide whether a `COMMIT` it just sent was irrevocable. A
full transmit queue is a definite refusal; a BLE notification with no completion
event is not.

**Nothing chooses a BLE role.** `candidate_open` receives a non-zero
`peer_endpoint_token` only on the side that received a `TRANSPORT_OFFER` — the
only object that carries one. So the peer that can be *found* advertises, and
the peer holding the token scans. That is
[`BLE-ACTIVATE-1`](../mcl-ble/spec/ble-activate-1.md) §2, arriving as data.

**`candidate_open` may answer `PENDING`.** Opening a bearer is not instantaneous
anywhere real. Answer `PENDING`, then call `mcl_machine_candidate_ready()` on
success or `mcl_machine_candidate_refused()` when the asynchronous attempt
fails.
Nothing is emitted on the bearer before that call.

## 07 Run the conformance tests

You do not have to trust this repository's CI. The release developer package
ships its machine-contract test and runs it locally:

```sh
ctest --test-dir build --output-on-failure
```

Maintainers working from all eight source repositories additionally run:

```sh
sh ../mcl-core/tools/local-gates.sh            # all eight, GCC + Clang + sanitizers
sh ../mcl-core/tools/check-reference-deployment.sh
sh ../mcl-ap/conformance/check-vectors.sh      # two receivers, one corpus
python3 ../mcl-core/conformance/independent/test_independent.py   # C4
```

[`../mcl-core/conformance/`](../mcl-core/conformance/) holds the immutable
major-1 vectors, the ICS, and an independent implementation in Python that
shares no code with the C.

## 08 Deployment and security considerations

**Pick a deployment profile, do not invent one.**
[`MCL-REFERENCE-DEPLOYMENT-1`](../mcl-core/deployments/MCL-REFERENCE-DEPLOYMENT-1.json)
is the one the SDK ships as a constant; a gate fails the build if the document
and the constant ever disagree. Two builders who choose differently do not meet.

Before you ship anything, know these four:

- **`source_ref` is not an identity.** It correlates frames within a contact and
  has no uniqueness property. Two unrelated builders may legally choose the
  same one.
- **`endpoint_token` is not an address or a credential.** It is a transaction
  selector, valid for one activation window, observable and replayable by
  anything in range.
- **`COMMIT` is irrevocable.** Once transmitted there is no rollback, which is
  why policy is asked *before* it.
- **Between `COMMIT` and `CONFIRM` the contact is quiesced.** Ordinary sends
  return `MCL_SDK_ERR_QUIESCED` and transmit nothing. Handle it; it is not a
  failure.

Then read [`BUILDER_GUIDE.md`](BUILDER_GUIDE.md) §6 onward for what is *not*
there yet, in the author's own words rather than as an absence you have to
notice.
