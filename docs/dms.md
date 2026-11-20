# Direct messages through a Pond (design, decided, not built)

**Status:** the owner has answered the open questions (the decisions are listed at the end). Nothing is built yet.
Puddle's side is in Puddle's `docs/pond-dms.md`.

## What it is for

Two members of a Pond can write to each other privately without being friends: no stream, no address exchanged, nothing
asked first. As on Discord, someone you meet in a channel can be messaged from their name. These conversations sit in
the Puddle's direct messages, each shown as **"Ann @ Lily Pad"**, so it is never mistaken for a conversation with a
friend (a stream), even with the same person.

What it promises:

- **The Pond relays, it doesn't store.** A message passes through the Pond's memory on its way and is never written to
  its disk. A Pond's admin holds nothing they can't see and moderate: no sealed text, no sealed files.
- **The Pond can't read them.** Each message is sealed end to end, from the sender's Puddle to the recipient's Puddle,
  with keys the Pond can't substitute.
- **The Pond can't forge them.** Each is signed by its author's key; the recipient's Puddle checks it.
- **It works when the recipient is away.** The *sender's* Puddle holds the message and sends it again when the
  recipient's Puddle is back, for up to 30 days. A Puddle is the owner's always-on computer, so it is the right place to
  wait; the Pond only says when the recipient is back.
- **The Pond sees who writes to whom**, when, and how much, not what. That is what lets it rate-limit; the app says so.

## How it works

### Keys: nothing new

A member's Puddle already gave the Pond its key chain when it joined (`/pond/v1/join`, and `/pond/v1/keys` after a roll).
The chain (Puddle Protocol part 01) holds the member's current Ed25519 signing key **and its X25519 encryption key**, and
it proves itself: the user id is the hash of the first key, and every later key is signed by the one before. So a sender
asks the Pond for the recipient's chain, checks it against the recipient's id (the member id it already knows), and seals
to the encryption key in it. A Pond that answered with another key would have to forge a chain for that id, which it
can't. The worst a Pond can do is withhold a message, which it could always do.

New route: `GET /pond/v1/members/{member}/chain` answers the member's chain as the Pond holds it.

### The letter

The sender's Puddle builds the **letter**, the message itself, signed by the sender:

```json
{"v": 1, "from": "<sender id>", "to": "<recipient id>", "pond": "<pond id>", "id": "<26 base32, made by the sender>",
 "ts": <ms>, "kid": "<hex SHA-256 of the signing key>", "kind": "text", "text": "…", "reply_to": "<id>"?,
 "files": [{"name", "type", "size", "sha256", "key"}]?,
 "sig": "<base64 Ed25519 over the rest, canonical JSON>"}
```

`pond` and `to` are in it so a letter can't be replayed to someone else, or through another Pond. Other kinds:
`friend_offer` (below) and `delivered` (receipts, below). It is sealed with HPKE exactly as part 09 seals a request body
(`DHKEM(X25519, HKDF-SHA256)`, `HKDF-SHA256`, `ChaCha20-Poly1305`), to the recipient's encryption key, with
`info` = `puddle-letter-v1\n<from>\n<to>\n<pond>\n<id>`.

### Relaying

```
POST /pond/v1/dms/send  {"to": "<recipient id>", "id": "<the letter's id>", "kid": "<hex SHA-256 of the recipient's encryption key>",
                         "enc": "<base64 HPKE enc>", "sealed": "<base64 of the sealed letter>"}
```

The Pond checks only what it can: that DMs are on in this Pond, that the sender and recipient are both members now,
that the recipient takes DMs here (below), the size (at most 16 KiB sealed), and the sender's rate. Then:

- **The recipient's Puddle is following the Pond's events now:** the Pond hands the letter to that open request, in
  memory, as a `dm` event for them alone (`{id, from, kid, enc, sealed, ts}`), and answers `200 {"relayed": true}`.
  The event is not written to the `events` table.
- **It isn't:** `200 {"relayed": false}`. The Pond keeps no letter, only, in memory, that this sender is waiting for
  this recipient. When the recipient's Puddle next follows, the Pond sends the sender a `dm_back {member}` event, and the
  sender's Puddle sends again. A Pond that restarted has forgotten who waits, so a sender also tries again on its own,
  every 15 minutes at first and less often later.

The recipient's Puddle opens the letter, checks the signature against the sender's chain (asked of the Pond once, then
kept), checks `to`, `pond` and `from` match, drops a repeat by `id`, keeps it, and sends a `delivered` letter back
(sealed and relayed the same way), so the sender knows it arrived. The sender's Puddle keeps trying until it has the
receipt or 30 days have passed; then the message says it wasn't delivered.

A letter the recipient's Puddle can't open (a key it has rolled past, see "Keys rolling"), or whose signature is wrong,
is dropped.

### Files

Files go with the letter, sealed too, and relayed, never stored. The letter names each file with its own random key
(`files[].key`, inside the sealed letter); the file is encrypted with that key (ChaCha20-Poly1305, in 64 KiB chunks) and
streamed through the Pond:

```
POST /pond/v1/dms/files/{letter id}/{n}   body: the encrypted file      (sender, once the recipient has the letter)
GET  /pond/v1/dms/files/{letter id}/{n}   body: the same bytes, as they arrive   (recipient)
```

The Pond joins the two requests and passes the bytes from one to the other as they come, holding a few chunks in memory
at most. The sender's upload waits (up to a minute) for the recipient to ask; if they don't, the sender tries again
later, as for the letter. The recipient checks `sha256` and `size`. Limits are the Pond's: at most 25 MiB a file and 10
files a letter by default; the admin may lower them, or turn files in DMs off (`pondctl dms files off`), which `/info`
says by leaving out the feature `dm_files`.

### Who may write to whom

- **The Pond's admin** turns DMs on or off for the whole Pond (`pondctl dms on|off`, on by default), which `/info` says
  with the feature `dms`. Only between members: someone who isn't a member now can't write or be written to.
- **Each member** says whether they take DMs in this Pond, from their Puddle: `POST /pond/v1/dms/settings {"accept": true|false}`
  (`true` by default). A send to someone who doesn't is `409 not_accepted`, and the app doesn't offer **Message** for
  them; this is not a secret, as on Discord.
- **One person** a member doesn't want to hear from, they block. Blocks are the Puddle's, below.

### Blocking

Blocks stay in the recipient's Puddle, as they do today, and the Pond never learns of them. A blocked person's letters
are relayed like any other; the blocker's Puddle drops them unread but still sends `delivered`, so the blocked sender
sees them delivered and is told nothing, which is how a Puddle's block already behaves (Puddle's `docs/streams.md`).
There are two kinds (Puddle's `docs/pond-dms.md`): one for direct messages, and the Puddle's block, which also hides
their messages in Ponds.

### Keys rolling

A member whose keys roll gives every Pond its new chain, as now (`/pond/v1/keys`). A sender whose `kid` names a key the
Pond knows is not the recipient's current one gets `409 stale_key` with the chain, takes the new key, and seals again.
A letter relayed in the moment of a roll may be sealed to the key just rolled past; the recipient's Puddle keeps its
previous encryption key for 7 days after a roll, only to open letters.

### Leaving, removal, bans

When either member leaves the Pond or is removed, nothing more can be sent between them through it, and senders waiting
for them stop. What each Puddle kept stays in its conversation, read-only, still called "Ann @ Lily Pad".

### Turning it into a friendship

Either side may offer to become friends from the conversation. A Pond never learns a member's address (a Puddle always
calls the Pond), so the offer travels **inside a letter**: a letter with `"kind": "friend_offer"`, carrying the sender's
`puddle://` link (address and sealing key, part 09) and a note. The recipient's Puddle shows it as an offer in the
conversation; accepting sends an ordinary stream request to that link (part 03), naming the offer so the offering Puddle
accepts it without asking its owner again. Declining sends nothing. Only the recipient learns the address, and only
because the sender chose to send it.

## Where it lives

- **In the protocol** (`ezp2p/protocol`): see decision 9 below, still open.
- **In `/pond/v1`**: an addition, so a protocol number and the features `dms` and `dm_files`. Routes
  `GET /pond/v1/members/{member}/chain`, `POST /pond/v1/dms/send`, `POST /pond/v1/dms/settings`,
  `POST|GET /pond/v1/dms/files/{letter}/{n}`; the events `dm` and `dm_back`, for one member and never stored.
- **In the Pond's database**: only `members.dms_accept` and the Pond's own settings. No letters, no files.
- **In Puddle**: a conversation kind of its own in the direct messages, labelled with the Pond, plus the waiting
  letters (Puddle's `docs/pond-dms.md`).

## Limits

- A sealed letter: 16 KiB (text up to 4000 characters, as a channel message).
- Files: 25 MiB each, 10 a letter, by default; the admin may lower them or turn them off.
- Each member: 1 letter a second on average, bursts of 10; at most 20 different recipients a day who have never
  written back, to make a Pond a poor tool for spam.
- A sender waits for a recipient's return for 30 days.

## Decisions (the owner's answers)

1. **Relay, not introduce.** The Pond relays between members. *Storing:* the Pond holds nothing once a message is
   delivered, and the answer to 5 is that it shouldn't hold messages at all, so the waiting happens in the sender's
   Puddle (see "Relaying"). The owner should confirm this reading.
2. **Friend offers:** yes, from either side.
3. **Blocks:** as Puddle's blocks behave today: kept by the Puddle, silent, and a blocked person's messages aren't shown
   in Ponds either.
4. **Who may write:** configurable. The admin turns DMs on or off for the Pond; DMs are only between current members;
   each member may turn them off for themselves in a Pond; one person is blocked. Two blocks: DMs only, and everywhere.
5. **The Pond sees who writes to whom**, for rate limits, and doesn't store messages, since its admin can't moderate what
   they can't see.
6. **Files:** yes, bundled in, relayed and never stored.
7. **30 days**, now how long a sender's Puddle keeps trying.
8. **Where they show:** in the Puddle's direct messages.
9. **Where the letter format goes in the protocol:** asked again, with the choice spelled out. *Recommendation:* a
   section of part 05 (Ponds), since only a Pond carries letters now that nothing stores them on the way.
