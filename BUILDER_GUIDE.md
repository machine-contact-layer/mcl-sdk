# Adding MCL to a machine

For the engineer wiring MCL into a product. It answers the ten questions a
builder actually asks, in order, and where the answer is "MCL does not do that
yet" it says so with the reason rather than leaving you to infer it from a
missing header.

`mcl-core/research/TWO_BUILDER_AUDIT.md` exists because this document did not.

> **If you have not run MCL yet, start with [`QUICKSTART.md`](QUICKSTART.md).**
> It gets you from a clone to two machines in contact in eight steps, using
> `mcl/machine.h` — the facade that owns the protocol choreography. This
> document is the layer underneath: what the facade is doing, and every question
> it does not answer for you.

## 0. What you are agreeing to build

MCL is a delivery layer for first contact and contact continuity. It is not an
autonomy stack, a credential authority, a fleet manager or a modem.

The split, before you write anything:

| | Supplies |
|---|---|
| **MCL** | Wire encoding, Link frames, negotiation, migration, the transport mappings, the conformance rules |
| **You** | Speaker, microphone, BLE stack, sockets, key storage, machine policy, application behaviour |
| **Your deployment** | Trust anchors, credential issuance, the deployment profile, local policy |

**Read this before designing anything around acoustic:** `MCL Stranger-Contact 1`
is claimable, and only **with a caveat**. `AP-BOOTSTRAP-1` and `BLE-ACTIVATE-1`
are both **Candidate**: normatively complete and implementable from their text.
The retained DFR1154/Android campaign now includes zero-prior migration with
explicit policy admission in both BLE orientations, using different platform
stacks. Later adapter fixes have their own revision-specific regression logs.
The later three-device campaign reached DFR/Android migration with Windows
participating acoustically, including an ignored competing ACCEPT. The original
three-party release invariant is satisfied; the later collision/traffic matrix
is informative robustness work, and public review remains open. The caveat therefore travels
in code as `MCL_CONFORMANCE_CAVEAT_BOOTSTRAP_CANDIDATE`, not only in prose. See
§4 for what that means for your product.

The earlier central-connection failures, extra-client allocation, and reversed
native-address defect remain retained negative evidence. The corrected image
tests address identity at boot and keeps activation off the MCL polling task;
the subsequent both-role lifecycle campaign is the current result. Do not read
the historical failure as the current lifecycle verdict.

## 1. How do I install MCL?

Use the self-contained `mcl-developer-sdk` release package. Its one CMake target
contains Wire, Link, and the high-level SDK; a product consumer does not clone
or install repositories in dependency order.

```sh
cmake -S mcl-developer-sdk -B build -DCMAKE_INSTALL_PREFIX=/your/prefix
cmake --build build && cmake --install build
```

Then from your own CMake:

```cmake
find_package(mcl_sdk REQUIRED)
target_link_libraries(my_machine PRIVATE mcl::mcl_sdk)
```

`packaging/verify-developer-sdk.sh` generates exactly that package, installs it
into an empty prefix, and builds a scratch consumer using only
`find_package(mcl_sdk)`. It also fails if the consumer reaches below
`mcl/machine.h` into Wire, Link, rendezvous, or migration APIs.

Everything protocol-facing is **freestanding C99**: no heap, no libc
dependency, caller-owned structs. It builds for a microcontroller because it was
built on one — see `mcl-ap/experiments/008-embedded-node/`.

## 2. How do I attach my transport?

> Through `mcl/machine.h` this is `mcl_platform_t::transport_send`, and the
> contract below is identical. The rest of this section applies either way.

One callback. The SDK turns semantic objects into canonical bytes and hands
them to you; getting those bytes onto a medium is yours.

```c
static int32_t my_tx(void *user, uint8_t transport_id,
                     const uint8_t *data, size_t size)
{
    /* transport_id says which bearer these bytes MUST leave on. */
    return my_socket_send(user, data, size);   /* 0, <0, or >0 -- see below */
}
```

**The return value is three-valued and it matters.**

```text
  0   accepted for transmission
< 0   DEFINITELY not transmitted. Nothing left this machine.
> 0   outcome UNKNOWN. It may or may not have left.
```

Do not collapse the last two. Committing a migration is irrevocable once the
bytes are transmitted, so the SDK must not enter that state for a frame the
transport is certain it never sent, and **must** enter it for one the transport
cannot vouch for, because the peer may have it. A full transmit queue is a
definite refusal; a BLE notification with no completion event is not. **If your
transport genuinely cannot tell, return > 0.** Claiming certainty you do not
have is the failure this parameter exists to prevent.

`transport_id` is a parameter because a migration spans two media at once. Do
not ignore it and infer the bearer from call order.

## 3. How do I announce myself?

> Through `mcl/machine.h` you do not: `mcl_machine_start()` announces, and the
> coordinator owns the cadence, the contention and the epoch. What follows is
> the layer underneath, for a builder who is not using the facade — a bearer
> with no shared medium, or an integration that has to place PRESENCE itself.

```c
mcl_node_config_t cfg = {0};
cfg.supported_wire_majors_mask = (1u << 1);   /* Stable major 1 */
cfg.tx_fn = my_tx;
cfg.user_ctx = &my_socket;
cfg.source_ref = my_contact_reference;
cfg.transport_id = MCL_CONTACT_TRANSPORT_IP;
cfg.role = MCL_CONTACT_ROLE_INITIATOR;

mcl_node_t node;
mcl_node_init(&node, &cfg);
```

Then send a `PRESENCE`. Two paths exist and the choice is real:

- `mcl_node_send_tier0()` — raw canonical Wire bytes. What a broadcast beacon
  wants, and what a bearer like acoustic carries directly.
- `mcl_node_send_framed_tier0()` — inside a Link frame, with a class, session
  reference and sequence. What a real contact session needs, and what the IP and
  BLE bindings carry.

`source_ref` correlates frames within a contact. **It is not an identity.** A
peer must never treat it as proof of who sent something.

## 4. How do I hear an unknown peer?

**This was the honest gap, and it is the one that has changed most.** What
follows replaces the earlier answer, which said acoustic rendezvous did not
exist yet and told you not to design around it.

### What exists now

```text
AP-BOOTSTRAP-1     Candidate   mcl-ap/spec/ap-bootstrap-1.md
    the acoustic bootstrap profile: waveform, objects, contention,
    solicitation epochs, timing constants

BLE-ACTIVATE-1     Candidate   mcl-ble/spec/ble-activate-1.md
    how two strangers who have agreed on BLE actually reach a connection

mcl/rendezvous.h   the coordinator that drives them
mcl/machine.h      the facade you should actually program against
```

So you no longer implement detection, PRESENCE, contention, ordered bearer
trial, OFFER/ACCEPT, activation, path validation or migration. You implement a
clock, randomness, a way to move bytes, a way to open a bearer and a policy
answer, and you receive one event. See [`QUICKSTART.md`](QUICKSTART.md) §06.

### What is still true

**Both profiles are Candidate, not Stable, and the reason is not a formality.**

`AP-BOOTSTRAP-1` and `BLE-ACTIVATE-1` remain Candidate because private physical
evidence is not independent external implementation or public review. The
DFR1154 and Android adapters have exchanged acoustic bootstrap
objects and reached two-device migration in both BLE roles. The retained
three-device campaign includes migration, and satisfies the original shared-air invariant. The later collision and
traffic stress matrix remains incomplete and is retained as informative work. Windows is an AP contention participant,
not a qualified BLE-ACTIVATE-1 port in both roles. The detailed record is in
`hardware/dfr1154-autonomous-node/runs/20260909-central-lifecycle/README.md`.
The original 3+ physical contention variant is closed; final release engineering
and public review are separate gates. Until
those pass, `MCL Stranger-Contact 1` is
**not guaranteed** between builders who never coordinate — and the caveat
travels in code, as `MCL_CONFORMANCE_CAVEAT_BOOTSTRAP_CANDIDATE`, rather than
only in prose.

**The Stable bearers still defer discovery, deliberately.** `IP-DATAGRAM`
assigns no port and defines no discovery; `BLE-GATT` makes advertising
explicitly optional, which is right for a profile about carriage over an
established connection. Two fully conformant implementations can therefore each
wait for the other to connect. That is the gap `BLE-ACTIVATE-1` closes for a
deployment that names BLE as a candidate bearer, without touching Stable
`BLE-GATT-1`.

**So: what should you design around today?**

- If your machines have a bearer in common already — a fleet network, a
  provisioned peer, a known address — use it, and treat acoustic rendezvous as
  something you gain later without changing your integration.
- If they genuinely have nothing in common, `MCL-REFERENCE-DEPLOYMENT-1` is the
  path, and you are adopting two Candidate profiles knowingly. They are
  normatively complete and implementable from their text; what they lack is a
  independent external implementation/review and a Stable profile assignment.

## 5. How do I select a common bearer, and move to it?

`TRANSPORT_OFFER` carries where to reach you on a candidate bearer;
`TRANSPORT_ACCEPT` answers. Then the migration runs:

```text
TRANSPORT_OFFER / ACCEPT        on the current transport
PATH_CHALLENGE / PATH_RESPONSE  on the CANDIDATE transport
COMMIT / CONFIRM                on the candidate transport
```

You drive it with `mcl_node_send_handoff()` and `mcl_node_apply_handoff()`. Two
things will bite you if you skip them:

**COMMIT is irrevocable.** Once it is sent there is no rollback. This is why §2's
three-valued return exists.

**Between COMMIT and CONFIRM the contact is QUIESCED.** Sending ordinary traffic
returns `MCL_SDK_ERR_QUIESCED` and transmits nothing, because the peer may
already have left the transport you still consider active. Handle that status;
do not treat it as a failure.

`endpoint_token` is resolved on the candidate transport. It is not an address,
not a credential, and not a capability.

**If you use `mcl_rdv_t`, two of these values are yours to allocate and it will
not guess for you.** `mcl_rdv_platform_t` has `allocate_session` and
`allocate_endpoint_token`, both optional:

| Leave NULL when | Supply when |
|---|---|
| you run one contact, with a fixed address on each bearer | you run a pool of contacts, or your address is per-transaction |

The built-in defaults are a hash of your own `source_ref` and a value read from
`mcl_rdv_config_t::bearer_endpoint_token[]`. Both are functions of *this
machine* and neither can be right in a pool: `session_ref` has to be distinct
across every contact you are running, and the coordinator has no view of the
others. On BLE the token has to select **one transaction**, because
`BLE-ACTIVATE-1` makes it the match key of the advertisement your peer scans
for — one static token for two concurrent activations advertises identically
for both.

A hook that returns non-zero, or writes a zero session, is a **refusal**, and it
is honoured: no acceptance is sent, no offer claims an address you did not mint,
and nothing of the coordinator's own is substituted. The peer sees silence,
retries, and eventually reports `NO_COMMON_BEARER` — which is the truth from its
side.

`allocate_endpoint_token` is called **once per transaction**, not once per
emission. A retransmitted offer carries the token the first one did, for the
same reason it carries the same `migration_ref`.

## 6. How do I authenticate the peer?

**You cannot, in MCL, today.** No cryptography is implemented in any repository:
no confidentiality, no peer authentication, no replay protection, and capability
negotiation is unauthenticated.

Your options now:

- Place MCL inside something that authenticates — DTLS, BLE Secure Connections,
  a private network. Legitimate, and what `mcl-core/SECURITY.md` recommends.
- Or accept unauthenticated contact deliberately, which is correct for open
  presence and hazard broadcast where you are addressing unknown listeners on
  purpose.

Understand what the first option does not buy you: if you choose DTLS with P-256
and another builder chooses something else, you have both "secured MCL" and
still cannot authenticate each other. A named security profile is post-v1
research; v1.0 deliberately does not select or imply one.

**Reception is never permission.** MCL keeps these strictly apart and so must
you:

```text
reception != identity != authenticity != authority != trust != obligation
```

## 7. Where do I obtain the trust root?

From your deployment, not from MCL. MCL never holds a private key and is not a
CA.

A deployment profile names what is mandatory in one place —
`mcl-core/spec/deployment-profile-v1.md`, with a worked example at
`mcl-core/deployments/MCL-REFERENCE-DEPLOYMENT-1.json`. Trust anchors and
credential formats are deliberately **not** in the v1 schema, because those
decisions are not made yet and a placeholder that later changes meaning is worse
than an absent field.

## 8. How do I expose what my machine can do?

Barely, in v1, and this is deliberate.

Stable Tier-0 is `PRESENCE`, `TRANSPORT_OFFER`, `TRANSPORT_ACCEPT`. You can say
you are here, propose a better bearer, and keep the contact alive across the
move. You **cannot** yet say what you are, what you can do, or what you want.

`HAZARD`, `REQUEST`, `AUTHORITY_CLAIM` and `DEGRADED_STATE` exist at Wire major
0 as **Candidate**: layouts frozen, meanings not. Two unrelated vendors would
not reliably act on them the same way, so v1 does not claim they would. Use them
inside your own fleet if you like; do not build cross-vendor behaviour on them.

## 9. How do I know my implementation conforms?

Run the check at init, and let it tell you what you may claim:

```c
#include "mcl/conformance.h"

mcl_build_capabilities_t caps = {0};
caps.wire_major_1 = 1; caps.link_major_1 = 1;
caps.stable_tier0_kernel = 1; caps.negotiation = 1;
caps.refusal_semantics = 1;
caps.transports_mask = (1u << MCL_CONTACT_TRANSPORT_IP);

mcl_conformance_report_t r;
mcl_conformance_evaluate(&caps, MCL_CONFORMANCE_BASE_1, &r);

if (r.claim_stands) {
    printf("claiming %s\n", mcl_conformance_layer_name(r.requested));
} else {
    uint32_t bit;
    printf("cannot claim %s; highest honest claim is %s\n",
           mcl_conformance_layer_name(r.requested),
           mcl_conformance_layer_name(r.attainable));
    for (bit = 1u; bit != 0u; bit <<= 1) {
        if (r.unmet & bit) {
            printf("  - %s\n", mcl_conformance_unmet_name(bit));
        }
    }
}
```

And against a deployment:

```c
mcl_deployment_requirements_t req = {0};
req.required_layer = MCL_CONFORMANCE_BASE_1;
req.mandatory_transports_mask = (1u << MCL_CONTACT_TRANSPORT_BLE);

mcl_deployment_report_t d;
mcl_deployment_check(&caps, &req, &d);
if (!d.satisfied) { /* surface it; do not continue silently */ }
```

**This checks consistency, not behaviour.** Every field is asserted by you and
nothing detects a false assertion. What it catches is claiming a layer you did
not wire up. Behaviour is tested by `mcl-core/conformance/`.

Three claims exist. Pick the honest one:

| Claim | Means |
|---|---|
| `MCL Base 1` | Given a shared bearer, we interoperate. **Claimable today.** |
| `MCL Stranger-Contact 1` | We can meet a machine we have never met. Claimable only with `MCL_CONFORMANCE_CAVEAT_BOOTSTRAP_CANDIDATE` until AP-BOOTSTRAP-1 and BLE-ACTIVATE-1 complete their independent physical qualification. |
| `MCL Secure-Stranger 1` | …and authenticate it. Reserved name, not specified. |

A machine with no microphone may truthfully claim Base 1 forever. That is a
first-class claim, not a degraded one — it is what most real deployments need.

## 10. What will change under me?

Stated so you can plan:

- **Wire major 1 and Link major 1 are frozen.** Bytes do not change without a
  new major.
- **v1.0 promises source compatibility, not ABI.** Recompile against a new
  release; do not assume a stable binary layout.
- **`AP-BOOTSTRAP-1` will be assigned a new profile identifier.** Profile 192 is
  Experimental Use and is never relabelled Stable, so anything you build on 192
  will be replaced. Do not ship it.
- **A `SECURITY` frame class and a feature bit will be assigned.** Your build
  will refuse them until you implement them, which is the designed behaviour and
  not a break.
- **Nothing outside this project has implemented or reviewed these
  specifications.** You may be the first. `mcl-core/REPORTING.md` is how you tell
  us what we got wrong, and `mcl-core/errata/` is where it goes.

## Where to look next

| You want | Read |
|---|---|
| What v1.0 claims and refuses to claim | `mcl-core/governance/V1_SCOPE.md` §5.9 |
| Why the layers exist | `mcl-core/spec/conformance-profiles-v1.md` |
| What a deployment selects | `mcl-core/spec/deployment-profile-v1.md` |
| Every specification and its status | `mcl-core/SPECIFICATION_INDEX.md` |
| What a clean-room implementer got wrong | `mcl-core/conformance/independent/SPEC_GAPS.md` |
| The honest gap analysis | `mcl-core/research/TWO_BUILDER_AUDIT.md` |

The latest bounded physical record is
`hardware/dfr1154-autonomous-node/runs/20260909-contention-closure/README.md`.

The owner review of 2026-09-10 restores the original Row 35 invariant; see
`../mcl-core/conformance/independent/20260910-private-rc/README.md`. Historical
campaign verdicts are retained with their original, stricter test scope.
