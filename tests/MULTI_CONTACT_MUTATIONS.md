# Multi-contact campaign — mutation results

`V1_SCOPE.md` §5.5, release gate item 9.

## Why this file exists

`test_multi_contact.c` passed on its first run, all nine cases. A campaign that
passes immediately is exactly the campaign most likely to be testing the harness
rather than the protocol — so each isolation mechanism it claims to exercise was
deliberately broken, one at a time, to check the campaign notices.

Two of the four mutations produced findings. One of them was a finding about the
campaign itself.

## Results

| # | Mutation | Site | Caught | Failures |
|--:|---|---|---|---:|
| 1 | Destination filtering always says "addressed to me" | `sdk.c`, `mcl_node_frame_addressed_elsewhere` | **yes** | 6, across 3 independent cases |
| 2 | `migration_ref` match removed on the **retransmission** path | `contact.c`, `mcl_contact_agree` duplicate branch | **NO — escaped** | 0 |
| 3 | `migration_ref` match removed on the **OFFERED** path | `contact.c`, `mcl_contact_agree` | **yes** | 1 |
| 4 | Mutation 2, re-run after the gap was closed | same as 2 | **yes** | 1 |

## The finding

**Mutation 2 escaped.** The endpoint-swap case (case 5) claimed to exercise the
`migration_ref` check, and it does — but only the copy of that check on the
`OFFERED` path. `mcl_contact_agree` has a *second* reference check on its
duplicate/retransmission branch, reached only once a contact has moved past
`OFFERED` into `AGREED`/`VALIDATING`/`VALIDATED`/`COMMITTING`. Nothing in the
campaign reached it.

That branch is the more dangerous of the two. It decides whether a delayed
acceptance belonging to *another contact* is mistaken for a retransmission of
this contact's own — arriving at a contact that is already validating, which is
precisely when a mistake is least recoverable.

Case 5 was extended to drive the contact into `VALIDATING` and then present the
other contact's reference. Mutation 4 confirms the gap is closed: the same
mutation that previously escaped now fails the campaign.

The campaign also now asserts the *positive* half — a genuine retransmission of
the contact's own acceptance is still idempotent and does not drag the contact
back to `AGREED` — so the refusal is specific rather than a blanket rejection
that would happen to pass a negative test.

## Reproducing

Each mutation is a one-line edit, built and run, then reverted:

```sh
# 1
#   in mcl_node_frame_addressed_elsewhere, insert: return 0;
# 2
#   in mcl_contact_agree duplicate branch:
#   if (migration_ref != contact->pending_migration_ref ||   ->   if (0 ||
# 3
#   in mcl_contact_agree OFFERED path: delete the migration_ref check
```

Then `cmake --build <dir> && ./mcl_sdk_test_multi_contact`.

**No mutation is committed.** `git status` on `mcl-link` and `mcl-sdk` was
verified clean of `MUTATION` markers before this file was written.

## What this does not establish

- **Not exhaustive.** Four mutations is a spot check on the mechanisms this
  campaign names, not coverage of `contact.c`.
- **Not physical evidence.** This is host-side software conformance. The
  over-air multi-contact case — several real peers on one radio — is separate
  and is not claimed here. The E4 dual-transport evidence covers one contact
  across two media, which is a different property.
