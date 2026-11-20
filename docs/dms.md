# Direct messages through a Pond (proposal, not built)

**Status:** a design for the owner to decide on. Nothing here is built. The questions that need an answer before any code
are at the end, each with a recommendation. Puddle's side is in Puddle's `docs/pond-dms.md`.

## What it is for

Two members of a Pond can write to each other privately without being friends: no stream, no address exchanged, nothing
asked first. As on Discord, someone you meet in a channel can be messaged from their name. Each such conversation is
shown as **"Ann @ Lily Pad"**, so it is never mistaken for a conversation with a friend (a stream), even with the same
person.

It must not weaken anything a Pond promises today:

- **The Pond can't read them.** Each message is sealed end to end, from the sender's Puddle to the recipient's Puddle,
  with keys the Pond can't substitute.
- **The Pond can't forge them.** Each is signed by its author's key; the recipient's Puddle checks it.
- **It works when the recipient is away**, from behind NAT or a Levee: the Pond holds what it can't yet deliver, and
  hands it over on the event stream every member's Puddle already follows.

## How it works

### Keys: nothing new

A member's Puddle already gave the Pond its key chain when it joined (`/pond/v1/join`, and `/pond/v1/keys` after a roll).
The chain (Puddle Protocol part 01) holds the member's current Ed25519 signing key **and its X25519 encryption key**, and
it proves itself: the user id is the hash of the first key, and every later key is signed by the one before. So a sender
asks the Pond for the recipient's chain, checks it against the recipient's id (the member id it already knows), and seals
to the encryption key in it. A Pond that answered with another key would have to forge a chain for that id, which it
can't. The worst a Pond can do is withhold a message, which it could always do.

New route: `GET /pond/v1/members/{member}/chain` answers the member's chain as the Pond holds it.

### Sending

The sender's Puddle builds the **letter**, the message itself, signed by the sender:

```json
{"v": 1, "from": "<sender id>", "to": "<recipient id>", "pond": "<pond id>", "id": "<26 base32, made by the sender>",
 "ts": <ms>, "kid": "<hex SHA-256 of the signing key>", "text": "…", "reply_to": "<id>"?,
 "sig": "<base64 Ed25519 over the rest, canonical JSON>"}
```

`pond` and `to` are in it so a letter can't be replayed to someone else, or through another Pond. It is sealed with
HPKE exactly as part 09 seals a request body (`DHKEM(X25519, HKDF-SHA256)`, `HKDF-SHA256`, `ChaCha20-Poly1305`), to the
recipient's encryption key, with `info` = `puddle-letter-v1\n<from>\n<to>\n<pond>\n<id>`, and sent:

```
POST /pond/v1/dms/send  {"to": "<recipient id>", "id": "<the letter's id>", "kid": "<hex SHA-256 of the recipient's encryption key>",
                         "enc": "<base64 HPKE enc>", "sealed": "<base64 of the sealed letter>"}
```

The Pond checks only what it can: that the sender and recipient are both members, that the recipient takes letters
(below), the size (at most 16 KiB sealed), and the sender's rate. It stores the sealed letter and answers `200 {id}`;
`id` being the sender's makes a second try safe, as a nonce does for a channel message.

### Delivering

The Pond adds an event for the recipient alone (`only_for`, as a role offer has today): kind `dm`, with `{id, from, kid,
enc, sealed, ts}`. The recipient's Puddle opens it, checks the signature against the sender's chain (asked of the Pond
once, then kept), checks `to`, `pond` and `from` match, and keeps it in a conversation of its own (Puddle's
`docs/pond-dms.md`). Then it says it has it: `POST /pond/v1/dms/received {ids}`, and the Pond deletes them.

What is never acknowledged is deleted after **30 days**, and the events that carry it after 7, like every event. So a
recipient's Puddle that was away more than a week fetches what waits for it with `GET /pond/v1/dms` instead: its
undelivered letters, oldest first.

A letter the recipient's Puddle can't open (a key it has rolled past, see "Keys rolling"), or whose signature is wrong,
is dropped, and acknowledged so it isn't offered again.

### Who may write to whom

Each member says whether they take letters in this Pond, from their Puddle: `POST /pond/v1/dms/settings {accept}` with
`everyone` (the default, as on Discord), `roles` (only operators and administrators), or `nobody`. A send to someone who
doesn't take it is `409 not_accepted`. The Pond's admin can turn letters off for the whole Pond (`pondctl dms off`),
which `/info` says by leaving out the feature `dms`.

### Blocking

A member blocks another for this Pond's letters: `POST /pond/v1/dms/block {member}` (and `/unblock`). From then on the
Pond keeps nothing they send to the blocker. **What the blocked sender is told is an open question** (below). The
blocker's Puddle also drops anything from them that is already on its way. A block is the recipient's own and the Pond
tells nobody of it; operators and administrators don't see it.

Blocking someone across the whole app (Puddle's `user/block`) blocks their letters in every Pond too: the Puddle sends
the block to each Pond it shares with them.

### Keys rolling

A member whose keys roll gives every Pond its new chain, as now (`/pond/v1/keys`). Letters sealed to the old encryption
key while that was happening can't be opened by a Puddle that no longer has it, so the Puddle needs to keep its previous
encryption key for a while after a roll (7 days, to match the events) just to open letters. Streams don't need this today
(a request sealed to a retired key is answered `409 stale_key` at once), so it is new work. A sender whose `kid` names a key older than
the Pond's copy of the chain gets `409 stale_key` with the chain, takes the new key, and seals again.

### Leaving, removal, bans

When either member leaves the Pond or is removed, nothing more can be sent between them through it, and letters waiting
for someone who is no longer a member are deleted. What each Puddle kept stays in its conversation, read-only, still
called "Ann @ Lily Pad".

### Turning it into a friendship

Either side may offer to become friends from the conversation. A Pond never learns a member's address (a Puddle always
calls the Pond), so the offer travels **inside a letter**: a letter with `"kind": "friend_offer"`, carrying the sender's
`puddle://` link (address and sealing key, part 09) and a note. The recipient's Puddle shows it as an offer in the
conversation; accepting sends an ordinary stream request to that link (part 03), naming the offer so the offering Puddle
accepts it without asking its owner again. Declining sends nothing. Only the recipient learns the address, and only
because the sender chose to send it.

### What the Pond sees

It can't read the letters or forge them, but it does see who writes to whom, when, and how much (the `from` and `to` on
each). That is what lets it enforce rate limits, blocks and "who may write". Hiding the sender from the Pond too ("sealed
sender") is possible but costs those, so it is an open question rather than the proposal.

## Where it lives

- **In the protocol** (`ezp2p/protocol`): a new section of part 05 (Ponds), "Letters between members", with the letter
  format and its sealing, reusing part 09's HPKE suite with its own `info` label (`puddle-letter-v1`) and part 02's
  signatures. The letter is meant to be the shape any sealed, stored-and-forwarded message takes later (a Levee holding
  messages, part 06; Drip, part 07), so the format is specified once, there, and part 05 says how a Pond carries it.
- **In `/pond/v1`**: an addition, so a protocol number and the feature `dms`. Routes `GET /pond/v1/members/{member}/chain`,
  `POST /pond/v1/dms/send`, `GET /pond/v1/dms`, `POST /pond/v1/dms/received`, `POST /pond/v1/dms/settings`,
  `POST /pond/v1/dms/block`, `/unblock`; the event `dm` (for one member).
- **In the Pond's database**: `letters` (`id`, `from`, `to`, `kid`, `enc`, `sealed`, `ts_ms`), `letter_settings`
  (`member`, `accept`), `letter_blocks` (`member`, `blocked`, `ts_ms`).
- **In Puddle**: a conversation kind of its own, kept and shown like a DM, labelled with the Pond (Puddle's
  `docs/pond-dms.md`).

## Limits (proposed)

- A sealed letter: 16 KiB (text up to 4000 characters, as a channel message).
- Each member: 1 letter a second on average, bursts of 10; at most 20 different recipients a day who have never
  written back, to make a Pond a poor tool for spam.
- Waiting for one recipient: at most 1000 letters, the oldest dropped first.

## Open questions for the owner

1. **Relayed and stored by the Pond (this proposal), or the Pond only introduces?** The alternative: the Pond passes
   each member's address to the other once both agree, and they talk over a stream from then on. That puts nothing on
   the Pond's disk but reveals addresses, needs both to agree first, and needs both online. *Recommendation: relayed
   and stored, sealed end to end, as above.*
2. **Can either side turn it into a friendship?** As above, the offer carries the offerer's `puddle://` link inside a
   sealed letter, so only the other member learns it. *Recommendation: yes, from either side, as above.*
3. **Blocking: what is the blocked sender told?** (a) Nothing: the Pond answers `200` as if delivered, so a block is
   never revealed (Signal's way); (b) "not delivered", without saying why (Discord's way). *Recommendation: (a).*
4. **Who may write by default?** `everyone`, `roles` or `nobody` as the default for a new member, and whether a Pond's
   admin may change that default for their Pond. *Recommendation: `everyone`, each member may change it, and the admin
   may only turn letters off for the whole Pond.*
5. **Should the Pond see who writes to whom?** Sealed sender hides `from` from the Pond, at the cost of rate limits and
   blocks per sender. *Recommendation: not now; say plainly in the app that the Pond sees who talks to whom, not what.*
6. **Files in letters.** They'd be sealed too, so the Pond's file store (which serves files in the clear to members)
   can't hold them as they are. *Recommendation: text only at first; sealed files as a second step.*
7. **How long the Pond holds a letter nobody fetched**: 30 days is proposed.
8. **Where the conversations show in the app**: in the one list of conversations with friends and groups, marked
   "@ Pond name" (proposed), or in the Pond's own sidebar, or both.
9. **The protocol's home for the letter format**: a section of part 05 (proposed), or a part of its own (10, "Letters")
   because Levee and Drip would use it too.
