# Direct messages through a Pond

**Status:** built (protocol 8, features `dms` and `dm_files`), 2026-11-21, as the owner decided (the decisions are at the
end). The letter itself, how it is signed, sealed and answered, is the protocol's part 10, "Letters"; this page is the
Pond's side. Puddle's side is in Puddle's `docs/pond-dms.md`. The code is `src/dms.cpp`.

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

The Pond keeps everything for direct messages in pondd's memory, under the Service's lock (`src/dms.cpp`): who is
following now, what waits to go to each member's Puddle on its next request for events, who wants to know when someone is
back, and the pieces of files on their way. A restart forgets all of it; Puddles allow for that by trying again.

- **Following.** A member's Puddle is *following* while it holds a request on `/pond/v1/events`, and for 15 seconds
  after one ends (`DM_PRESENT_MS`), so the moment between two requests doesn't count as away.
- **A letter** (`POST /pond/v1/dms/send`): checked for what the Pond can check (DMs on here, both members now, the
  recipient takes them, the size, the rate), then, if the recipient is following, put in their inbox and their waiting
  request woken: it goes out as the event `dm`, with no number, and the cursor doesn't move. `{"relayed": true}`. If not,
  `{"relayed": false}`, and the sender is noted as waiting for them; when their Puddle next asks for events, each waiting
  sender gets `dm_back`. A letter put in the inbox of a Puddle that had just gone is handed over when it comes back, or
  lost if pondd restarts first; the sender sends it again when no receipt comes.
- **A piece of a file** (`GET /pond/v1/dms/files/{letter}/{n}/{piece}?from=`): the recipient's request waits (30 s) while
  the sender's Puddle is sent `dm_want`; its `POST .../{piece}?to=` hands the bytes to the waiting request. Nothing is
  written to disk. At most 4096 pieces wait across the Pond.
- **Whether a member takes them**: `members.dms` (the one thing kept in the database), changed by `POST
  /pond/v1/dms/settings`; a `member` event tells everyone. The admin's switches are the settings `dms` and
  `dms.files_max` (`pondctl dms on|off`, `pondctl dms files max-size`), in `pond.dms` `{on, files_max}`.
- **Leaving**: a member removed or gone is forgotten: their inbox, and who waits for them.

## Where it lives

- **In the protocol** (`ezp2p/protocol`): the letter (its fields, signature, sealing, receipts and files) is part 10,
  "Letters", so a Levee (part 06) or Drip (part 07) can carry the same thing later; part 05 says how a Pond relays them.
- **In `/pond/v1`**: an addition, so a protocol number and the features `dms` and `dm_files`. Routes
  `GET /pond/v1/members/{member}/chain`, `POST /pond/v1/dms/send`, `POST /pond/v1/dms/settings`,
  `GET|POST /pond/v1/dms/files/{letter}/{n}/{piece}`; the events `dm`, `dm_back` and `dm_want`, for one member and never
  stored.
- **In the Pond's database**: only `members.dms` and the Pond's own settings. No letters, no files.
- **In Puddle**: a conversation kind of its own in the direct messages, labelled with the Pond, plus the waiting
  letters (Puddle's `docs/pond-dms.md`).

## Limits

- A sealed letter: 128 KiB as base64 (a message of 4000 characters with ten files' previews fits).
- Files: 25 MiB each, 10 a letter, by default; the admin may lower them or turn them off.
- Each member: 1 letter a second on average, bursts of 10; at most 20 different recipients a day who have never
  written to them, to make a Pond a poor tool for spam (counted in memory: a restart starts the day afresh).
- Waiting in one member's inbox: 500; senders waiting for one member: 1000.
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
9. **Where the letter format goes in the protocol:** a part of its own, part 10 "Letters", for Levee and Drip to reuse.
