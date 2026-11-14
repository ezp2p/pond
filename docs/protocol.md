# Pond and the Puddle Protocol

The wire every ezp2p program speaks is specified in **`ezp2p/protocol`** (check it out beside this repository:
`../protocol`). This file says which parts a Pond implements, and what the protocol now asks of it. Pond shares no code
with Puddle on purpose; the protocol's specification and test vectors are how the two stay in agreement
(`../protocol/decisions.md`).

| Protocol part | Pond | Where |
|---|---|---|
| 00 principles: `404` for every failed check, GET/POST only, versioned paths, `protocol` and `features` | built | `src/routes.cpp`, `docs/design.md` "Versions" |
| 01 identity: a member's key chain | built (written from Puddle's documents) | `src/identity.cpp` |
| 01 Pond ids | built | `src/key.cpp` |
| 02 request signatures | built (written from Puddle's documents) | `src/reqsig.cpp` |
| 02 answer receipts | **not yet**: see below | |
| 05 ponds | built | `docs/design.md`, `src/routes.cpp` |
| 06 Levee | nothing to do: a member's Puddle always makes the connection, so it works through a Levee as it is | |

## What the protocol asks of Pond next

1. **Check itself against the test vectors.** A test that loads `../protocol/vectors/signatures.json` and authenticates
   each `requests` entry with `src/reqsig.cpp` (as `tests/identity_test.cpp` does with chains today), skipping when there
   is no protocol checkout, as `tests/e2e_test.cpp` skips without a Puddle. The vectors were made by Puddle; a Pond that
   passes them reads signatures exactly as Puddle writes them.
2. **Sign receipts** (spec 05, proposed as protocol 2 of `/pond/v1` with the feature `receipts`). On every answer to an
   authenticated member, add the same `X-Puddle-Receipt` header Puddles send (spec 02), signed by the Pond's key over
   its own TLS certificate's SHA-256, and list `receipts` in `features`. Members' Puddles then stop pinning the Pond's
   certificate, so the Pond can renew it (Let's Encrypt) without cutting everyone off. `docs/design.md` "Not yet" lists
   that problem; this is the fix.
3. **`api/pond.yaml`**, which the README points to, is not in this branch; the routes are listed in `docs/design.md`
   only. It should be written (or the link removed), so the protocol can point to one list of Pond routes.

Later, from the protocol's notes (spec 05): messages signed by their authors, so a Pond cannot forge what a member said.
