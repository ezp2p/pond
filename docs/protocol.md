# Pond and the Puddle Protocol

The wire every ezp2p program speaks is specified in **`ezp2p/protocol`**, a submodule here at `third_party/protocol`.
This file says which parts a Pond implements, and what the protocol now asks of it. Since 2026-11-21 the bytes themselves
(ids, chains, request signatures, receipts, sealing) come from its library, **libpuddle-proto** (`third_party/protocol/lib`),
which Puddle and Levee use too; `src/encoding.cpp`, `identity.cpp`, `reqsig.cpp` and `seal.cpp` are the Pond's faces on
it (`third_party/protocol/decisions.md`).

| Protocol part | Pond | Where |
|---|---|---|
| 00 principles: `404` for every failed check, GET/POST only, versioned paths, `protocol` and `features` | built | `src/routes.cpp`, `docs/design.md` "Versions" |
| 01 identity: a member's key chain | built, by libpuddle-proto | `src/identity.cpp` |
| 01 Pond ids | built | `src/key.cpp` |
| 02 request signatures | built, by libpuddle-proto | `src/reqsig.cpp` |
| 02 answer receipts | built (protocol 3, feature `receipts`) | `src/reqsig.cpp`, `src/routes.cpp`, `docs/design.md` "Receipts" |
| test vectors | checked on every build: `tests/protocol_test.cpp` (from the submodule) | `vectors/signatures.json` |
| 05 ponds | built | `docs/design.md`, `src/routes.cpp` |
| 06 Levee | nothing to do: a member's Puddle always makes the connection, so it works through a Levee as it is | |

## What the protocol asks of Pond next

Done: the test vectors, receipts, and `api/pond.yaml`.

- **Checks itself against the test vectors.** `tests/protocol_test.cpp` authenticates each request in
  `third_party/protocol/vectors/signatures.json` with `src/reqsig.cpp`, and checks each receipt with the same rules a Puddle
  uses (`POND_PROTOCOL_DIR` points elsewhere).
- **Signs receipts** (spec 05, protocol 3 of `/pond/v1`, feature `receipts`). Every answer to an authenticated member
  carries `X-Puddle-Receipt`, signed by `pond.key` over the Pond's own certificate (`docs/design.md`, "Receipts"), and
  `/info` gives the key (`pond.key`). Puddle checks them and stops pinning, so a Pond can renew its certificate.
  `tests/e2e_test.cpp` shows both sides: a Puddle proving a renewed certificate and carrying on, and refusing a Pond
  with another key at the same address.
- **`api/pond.yaml`** is in this repository again (it had only reached doco).

Later, from the protocol's notes (spec 05): messages signed by their authors, so a Pond cannot forge what a member said.
