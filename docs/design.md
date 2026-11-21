# Pond design (first cut)

A **Pond** is a community server, like a Discord server: someone runs it, it has text channels, and people join with
an invite link. It is not a Puddle. Its members are Puddles, and each one connects to it with the same key it uses
everywhere else. The routes are in `api/pond.yaml`.

This first cut does text channels, and files in messages (see "Files"). Voice channels have a page of their own,
[voice.md](voice.md). Editing and deleting messages, and moderation beyond removing and banning members, come later (see
"Not yet" at the end).

## What runs

- **`pondd`**: the service. It has no UI and serves HTTPS on one port. It reads `pond.yaml`.
- **`pondctl`**: the admin tool. It works on the same database and data folder as `pondd` and needs the same config.
  It sets up a new Pond, names it, manages channels and invites, and lists or removes members. `pondd` does not
  have to be stopped while it runs.
- **MariaDB** holds everything: the Pond's settings, channels, members, invites, messages and the event log.
- **Redis** is not used yet. It comes in when one Pond runs as several `pondd` processes and they need to tell each
  other about new events, or share rate limits. Until then the events table is the only way news travels.

The Pond's own files live in `data_dir`. They are its key (`pond.key`, mode 0600), the TLS certificate and key
(`cert.pem`, `key.pem`), the files members send (`files/`, see "Files"), and pictures (`pictures/`: the icon and members'
profile pictures, see "Pictures"). The certificate is made self-signed on first start, as a Puddle does, unless the operator has
put their own there.

The Pond is its own program and shares no code with Puddle. What the two must agree on is the protocol (see
"Versions"), above all the parts that prove who a member is: key chains and request signatures. The Pond checks those
in `src/identity.cpp` and `src/reqsig.cpp`, written from Puddle's documentation (its `docs/identity.md`, "The chain
file", and `docs/streams.md`, "Request signatures"). `tests/identity_test.cpp` checks them on their own, and
`tests/e2e_test.cpp` against real Puddles.

It is built on Drogon: HTTPS on a few threads (`threads` in `pond.yaml`), with no thread per connection, so a request
held open for events costs a socket and a little memory. Handlers are C++20 coroutines, and MariaDB is reached through
Drogon's asynchronous client, so a request waiting on the database holds no thread either. CMake fetches Drogon at a
fixed release and builds it with MariaDB and Redis support, with one small fix of ours (`patches/`).

## Who a Pond is

A Pond has an Ed25519 key, made by `pondctl init`. Its **id** is made the way a Puddle's user id is: the base32 of the
SHA-256 of the public key, so 52 characters. Puddles sign every request for that id, so a request meant for one Pond
can't be replayed to another.

There is no recovery phrase and no key rolling. A Pond that loses `pond.key` is a new Pond.

The key also signs the Pond's answers, and a key derived from it seals them (see "Receipts and sealing"), so it is the
key, not the TLS certificate, that members trust.
The certificate can change freely; `pond.key` must not.

## Invite links

```
pond://pond.example.org?invite=k3v8y2m4q7c9x5t1w6n0r3z8ab
pond://203.0.113.7:8443?invite=...
```

- The address part follows the same rules as a Puddle address: a host or IP, the port defaulting to 443, and no path.
- The token is 26 characters of base32 (16 random bytes), the same form as a Puddle invite.
- `pondctl invite create` makes an invite. It can have a number of uses (`--uses 10`), an expiry (`--expires 7d`, or a
  date), both, or neither, in which case it lasts until revoked. Operators and administrators make, list and revoke them
  from the app too (`/pond/v1/admin/invites`, below), each answered with its link.
- Joining spends one use. Someone who is already a member spends nothing.
- A used-up, expired, revoked or unknown token is answered `404`, like any other failed check, so a guessed token
  tells the guesser nothing.

## Joining

The member's Puddle sends `POST /pond/v1/join`. It is signed for recipient `*`, because the Puddle does not know
the Pond's id yet; that is the only route that accepts this. The body is:

```json
{ "invite": "<token>", "chain": "<the Puddle's key chain>", "profile": { ...the profile chosen for this Pond... } }
```

The Pond checks the chain and that the request is signed by its newest key. It spends the invite (one atomic
`UPDATE`), and records the member with their id, current key and profile. It answers `200` with the Pond (id, name
and description), the member as the Pond sees them, the channels, and the newest event number (`cursor`).

The answer is sealed and signed (see "Receipts and sealing") and carries the Pond's public key in `pond.key` and its
sealing key in `pond.seal_key`; the Puddle keeps it, and checks
that its SHA-256 is the Pond's id. From then on it holds the Pond to that key.

## Receipts and sealing

TLS only carries what members and the Pond say (the Puddle Protocol, `spec/09-sealing.md`): no certificate is named,
pinned or compared, so a Pond can sit behind any reverse proxy, on any certificate, renewed whenever, with nothing to
copy or restart.

**Sealing.** The Pond has a sealing key (X25519), derived from `pond.key`'s seed (HKDF-SHA256, info `pond-seal-v1`), so
there is no other file to keep. It is in `/info` and the join's answer as `pond.seal_key`, and in every invite as `k`
(`pond://pond.example.org?invite=<token>&k=<base32>`). Every request is sealed to it (`X-Puddle-Seal`, `src/seal.cpp`:
HPKE, RFC 9180), and the answer goes back sealed under a key exported from that request, so only that member can read it.
A request that can't be opened is the plain `404`; one from a member that wasn't sealed is `400 not_sealed`.

**Receipts.** Every answer to a member carries the Pond's receipt, the same header a Puddle puts on its answers
(`spec/02-signatures.md`; `src/reqsig.cpp`):

    X-Puddle-Receipt: v2 id=<pond id>; kid=<hex SHA-256 of its key>; sig=<base64>

signed by `pond.key` over the request's nonce, the status and the SHA-256 of the body exactly as sent (sealed). So does
the answer to a join that worked. Nothing that anyone could be sent carries one: not the `404` (a failed check, a refused
invite) nor a join that went wrong, so a receipt never says to a stranger which Pond lives at an address.

What they are for: a member's Puddle believes an answer only with a good receipt, so someone in the middle can pass
requests on but can't read them or answer for the Pond. An answer that can't be proven stops the Puddle talking to that
address (its status is `not_proven`) until one can.

**A member we removed.** A request the Pond opened, from someone who is not (or no longer) a member, gets the same `404`
as an unknown path, but sealed under that request's key and unsigned. Only this Pond could seal it, so the Puddle can tell
it really was removed; a plain `404` from someone else at the address proves nothing.

A Pond with this says so in `/info` (protocol 5, features `receipts` and `sealed`). Puddles that joined before (no
`seal_key` kept) have to join again with a new invite.

## Being a member

Every other route needs a request signed for the Pond's id, by the member's current key. A request from anyone who is
not a member is `404`.

When a member's Puddle rolls its keys, it sends its new chain to `POST /pond/v1/keys`. That request is signed by the
new key and proven by the chain it carries. The chain must belong to the same id, and must be at least as new as the
one held.

| Route | What it does |
|---|---|
| `GET /pond/v1/info` | The Pond, the member, the channels and the cursor (the same answer as joining) |
| `GET /pond/v1/channels/{channel}/messages?before=<id>&limit=<n>` | History, newest first: 50 by default, 100 at most |
| `POST /pond/v1/channels/{channel}/send` | `{text, nonce}`: posts a message and answers with it |
| `GET /pond/v1/events?after=<n>` | What happened after event `n`, waiting up to 25 seconds for something to happen |
| `GET /pond/v1/members` | Everyone in the Pond, with the profile each one chose for it |
| `POST /pond/v1/profile` | `{profile}`: change how the member appears here (`null` shows nothing) |
| `POST /pond/v1/leave` | Stop being a member. Their messages stay. |
| `POST /pond/v1/files/{file}` | `{size, chunks}`: begin sending a file; answers which chunks are still wanted |
| `POST /pond/v1/files/{file}/chunks/{n}` | One chunk's bytes |
| `GET /pond/v1/files/{file}/chunks` | A file's chunk hashes, as bytes |
| `GET /pond/v1/files/{file}/chunks/{n}` | One chunk, as bytes |
| `POST /pond/v1/pictures` | A picture's bytes, for the member's profile: answers `{hash, mime, size}` (see "Pictures") |
| `GET /pond/v1/pictures/{hash}` | A picture the members are shown: the icon, or one in a member's profile |

Message text is 1 to 4000 characters, with surrounding whitespace removed, or may be empty when the message carries
files. `nonce` is up to 32 characters chosen by
the sender. A second send with the same nonce from the same member answers with the first message and posts nothing,
so a Puddle that lost an answer can safely send again.

## Events

Every change the members need to hear about is a row in the `events` table, with an increasing number. The kinds are:

- `message`: a new message, which is the message itself.
- `channel`: a channel was made or changed, which is the channel itself.
- `channel_deleted`: a channel was deleted (`{id}`).
- `member`: someone joined or changed their profile, which is the member.
- `member_left`: someone left or was removed (`{id}`).
- `pond`: the Pond's name, description or icon changed, or the admin changed what files may be sent.
- `voice`, `voice_left`: someone came into a voice channel, changed how they are in it, or left ([voice.md](voice.md)).

A member's Puddle holds one request open on `/pond/v1/events?after=<cursor>` at a time. The Pond answers as soon as
there is anything after the cursor, up to 200 events at a time, or with an empty list after 25 seconds. The Puddle then
asks again from the new cursor. This works through a Levee and from behind NAT, because the Puddle always makes the
connection.

Events are kept for 7 days. If a cursor is older than the oldest event kept, the answer is `{"reset": true}`, and the
Puddle fetches `/info` and recent history again instead of replaying.

Inside `pondd`, a timer checks the newest event number twice a second and wakes the waiting requests when it
changes. That way a change made by `pondctl`, or by another `pondd` later, is seen without anyone telling it.

Events are committed in the order of their ids. Each transaction that adds one first locks the same row
(`settings.event_lock`), and holds it until it commits. Without that, two transactions could commit out of order: a
member could be handed event 11 while event 10 was still uncommitted, move its cursor past 10, and never see it.

## Files

Members send files with messages: photos, videos, anything. A file is kept once by the Pond, on disk, and each
member's Puddle fetches it from the Pond. This is built to work the way files already work between Puddles (Puddle's
`docs/files.md`), so a Puddle reuses its file store for both, and the same file has the same id everywhere.

### The shape of a file

The same as between Puddles. A file is split into **chunks of 1 MiB** (the last one shorter). Each chunk has a
SHA-256, and the file's **id** is the SHA-256 of all the chunk hashes, one after another. So:

- a Puddle that has a file already knows its id and chunk hashes, and can send it to a Pond without hashing it again;
- the Pond checks each chunk against its hash as it arrives, and throws away a bad one alone;
- a send or a fetch that stops picks up where it left off;
- the same file sent twice, by anyone, is kept once.

### What the admin chooses

`pondctl files max-size 50M` sets the largest file members may send; `pondctl files max-size off` stops files
altogether. The default is **25 MiB**, and the most a Pond takes is 4 GiB. The setting is kept in `settings`
(`files.max_size`). It is said to members in the Pond object of `/info` and joining:

```json
"pond": {"id": "…", "name": "…", "description": "…",
         "files": {"max_size": 26214400, "chunk_size": 1048576, "per_message": 10}}
```

Changing it adds a `pond` event, so every member's Puddle hears of it at once. `/info` also lists `files` in
`features`, so a Puddle knows this Pond has files at all; an older Pond without them says nothing, and a Puddle sends
it none.

`pondctl files` shows the limit, and how many files are kept and the room they take.

Lowering the limit changes nothing already sent: those files stay, and can still be fetched.

### Sending one

1. `POST /pond/v1/files/{id}` with `{size, chunks}`, the hashes as base64. The Pond checks that they hash to the id and
   fit the size, and that the size is within the limit (`413 too_large` with `max`, or `409 files_off`). It records
   the file and answers `{"missing": [0, 1, 2]}`: the chunks it still wants. A file it has whole already answers
   `{"missing": []}`, so nothing is sent twice.
2. `POST /pond/v1/files/{id}/chunks/{n}` with each missing chunk's bytes. A chunk of the wrong length, or that does not
   hash to what step 1 said, is `400 bad_chunk`. When the last one arrives, the file is whole.
3. `POST /pond/v1/channels/{channel}/send` with `files`: `[{id, name, mime, w?, h?, duration_ms?, preview?}]`, and
   `text`, which may then be empty. Every file must be whole on the Pond (`409 no_file` otherwise). The message, and
   its `message` event, carry each file with the size the Pond has for it.

What a send says of a file is cleaned: the name loses control characters and may not hold `/` or `\` (`bad_file`),
a type that is not `type/subtype` becomes `application/octet-stream`, a preview must be base64 of at most 1400
characters (a picture of at most 1 KiB, as Puddles make them), and a width without a height is dropped. The name and
type are the sender's word: a Puddle checks the bytes before showing anything as a picture.

Any member may send a chunk of any file that has been begun, since each one is checked against its hash. The uploader
is recorded only so that they may fetch their own file before a message carries it.

### Fetching one

`GET /pond/v1/files/{id}/chunks` gives the hashes (32 bytes each, as bytes), then `GET .../chunks/{n}` each chunk.
A member may fetch a file only if it is whole and a message in a channel that has not been deleted carries it (or
they sent it). Anything else is `404 no_file`, so a guessed id says nothing about what the Pond holds.

### On disk, and letting go

```
data_dir/files/<id>             a whole file
data_dir/files/partial/<id>     one arriving: made at its full size, each chunk written where it goes
```

The database knows the rest: which chunks are here (`files.have`), and which messages carry which files
(`message_files`). The last chunk to arrive moves the file from `partial/` to `files/`.

Once an hour, files that no message carries and that were begun over a day ago are thrown away: uploads never sent,
or never finished. A send holds a shared lock on each file it carries until it commits, so a file is never taken away
under a message being posted. Files a message carries are kept for as long as the Pond is; messages cannot be deleted
yet, and a deleted channel keeps its messages (and so their files) in the database.

### Why it is built this way

- **Chunks and ids as Puddles have them.** A Puddle already splits, hashes and resumes files this way, and keeps them
  in one store by id. Making the Pond speak the same shape means no new hashing on either side, a Puddle that already
  has a file (it sent it, or saw it in another chat) never fetches it again, and the Pond can check every chunk.
- **On disk, not in MariaDB.** Files can be large and are read a chunk at a time; the file system does that well, and
  it keeps the database small enough to back up easily. The database holds only what must be consistent with the
  messages.
- **The Pond keeps the file; members do not serve it to each other.** Members of a Pond are strangers, mostly offline,
  and often behind NAT. A Pond is meant to be the place that is always up, so it serves the bytes, as a Discord server
  does. This costs the Pond's operator disk and bandwidth, which is why the size is theirs to choose.
- **Sent before the message.** A message only ever names files the Pond has whole, so nobody sees a message whose
  file never arrives.
- **A rate per member on chunks.** Fetching and sending are what cost a Pond bandwidth; a limit per member keeps one
  member from using all of it.

### A Puddle's side

A Puddle keeps the files of the messages it keeps (the newest 200 of each channel), so scrolling through a channel
doesn't wait on downloads. It fetches each one by itself up to its owner's `files.auto_download`, newest messages
first, and anything larger, or in older messages, when the owner asks. This is described in Puddle's `docs/ponds.md`,
"Files".

### Open questions

- **A cap on the room files take.** Only the size of each file is limited now; a busy Pond's disk fills without
  limit. A total, with the oldest files let go first (their messages kept, saying the file is gone), would bound it.
- **Who may send files.** Every member may. Once roles exist, this should be a permission, per channel.
- **Kinds of file.** Anything is accepted. An admin might want only pictures, or to refuse some types.
- **Deleting.** When messages can be deleted, a file goes with the last message carrying it, as in a Puddle.
- **Thumbnails.** A member scrolling a channel full of large photos fetches each whole. A smaller copy made by the
  sender (as the preview is now, but larger) would save bandwidth.
- **Fetching through a link.** A file is only reachable through a signed request by a member. Sharing a file outside
  the Pond (a public link) is not planned.
- **Bandwidth per Pond.** The rate limit is per member. A Pond with many members can still use all of its uplink; a
  total limit, or a queue, may be needed.

## Pictures

A Pond has an icon, shown on its button in each member's window, and members show the pictures in the profile they chose for
it (avatar, banner, background), as friends do between Puddles. Both are **pictures**: an image of at most 1 MiB, PNG,
JPEG, GIF or WebP (judged by its first bytes), kept once in `data_dir/pictures/<sha256>`.

The hash is the one a Puddle already names a picture by in a profile (Puddle's `docs/profiles.md`: `{hash, mime, size}`), so
nothing new is said in a profile:

1. A member's Puddle sends each picture of the profile it shows this Pond: `POST /pond/v1/pictures` with the bytes. The Pond
   answers `{hash, mime, size}`; one it has already costs nothing to send again.
2. It then shows the Pond the profile, naming the pictures as it would to a friend (`POST /pond/v1/profile`, or the join).
3. Every other member's Puddle fetches a picture it does not hold with `GET /pond/v1/pictures/{hash}`, checks the hash, size
   and kind against what the profile said, and keeps it with its friends' pictures.

The icon is set by an operator or administrator from the app (`POST /pond/v1/admin/icon {hash}`, a picture sent first, or
`{hash: null}` for none), or by `pondctl icon <file>` (`pondctl icon off`). It is kept as the setting `icon`, and said in
the `pond` object of `/info` and of the `pond` event as `icon: {hash, mime, size}` (or `null`), so every member's Puddle
hears of a change at once.

A member may fetch only a picture that is shown: the icon, or one named in the profile of someone who is a member now.
Anything else is `404 no_picture`, so a guessed hash says nothing of what the Pond holds. A picture nobody is shown is let go
an hour or so after it is a day old (sent and never used, or replaced). Sending and fetching count against the member's
rate for file chunks.

Why not files: a file is chunked, held by the messages that carry it, and limited by the admin's `files.max_size`, which
may be off. A picture is small, one request, and must work on a Pond that takes no files.

## Roles: operators and administrators

Besides the person with a shell on the server (who runs `pondctl`), a Pond has members it trusts to run it from inside
the app. There are two such roles, above an ordinary member:

| Role | Who | May |
|---|---|---|
| **Operator** | The people running the Pond. Several may be. | Everything an administrator may, and act on administrators. Later, the Pond's own settings (what is now only in `pondctl`, short of the server: the database, ports, certificates stay in `pond.yaml`). |
| **Administrator** | People trusted to look after it. | Everything inside the Pond: channels, removing and banning members, the Pond's name. Not the system: they cannot touch operators, or settings an operator keeps. |
| Member | Everyone else | Read, write, send files. |

What exactly separates an operator from an administrator beyond rank is left to settle as each setting comes up; today
it is only that rank (see "Open questions").

### Becoming one: an offer, accepted

A role is never just given. The admin on the server offers it, and the member accepts it in their app:

1. The Pond is stood up, and its admin joins it with an invite from `pondctl invite create`, as anyone does. A Pond is
   invite only; there is no other way in.
2. `pondctl member offer <member> operator` (or `administrator`). `<member>` is their user id, or part of their
   display name; the Pond never learns a member's Puddle address, since Puddles always call the Pond, so the id is
   what names them. The app shows a member their own id ("Copy my id" in the Pond's menu).
3. `pondctl` shows who that is (id, display name, when they joined) and asks: `Offer operator to Ann (ab3k…)? [y/N]`.
4. The Pond records the offer and sends the member an event only they receive (`role_offer`). Their Puddle shows it:
   "Dev Pond invites you to be an operator", with Accept and Decline.
5. Accepting (`POST /pond/v1/roles/answer {offer, accept}`) gives them the role; every member hears of it (a `member`
   event, now carrying `role`). Declining drops the offer. An offer not answered in 7 days lapses.

`pondctl member role <member> member` takes a role away at once (no offer needed to step down), and
`pondctl member list` shows each member's role. The server's admin can always do anything; nothing in the app can
lock them out.

Why an offer rather than granting outright: the member sees it happen and agrees, a mistyped id gives nobody power
they did not ask for, and it uses the same event stream the Puddle already follows, so it reaches a Puddle behind NAT
or a Levee like everything else.

### Events for one member

An offer is for one member, so its event must not reach the others. Events gain a column, `only_for`: an event with
one is given only to that member's requests for events (the others skip its id, which is harmless: cursors only need
to increase). `/info` also says the member's open offer, if any, so a Puddle that missed the event (a reset) still
sees it.

### What they do from the app

Operators and administrators call admin routes, signed like everything else. The Pond looks up the sender's role; a
member without one gets the same `404` as any refusal.

| Route | Does |
|---|---|
| `POST /pond/v1/admin/pond` | `{name?, description?}` |
| `POST /pond/v1/admin/icon` | `{hash}`: a picture sent first, or `null` for no icon (see "Pictures") |
| `POST /pond/v1/admin/channels/create` | `{name, topic?}` |
| `POST /pond/v1/admin/channels/{channel}/edit` | `{name?, topic?}` |
| `POST /pond/v1/admin/channels/{channel}/delete` | |
| `POST /pond/v1/admin/members/{member}/remove` | Kick: they are out, and may come back with an invite |
| `POST /pond/v1/admin/members/{member}/ban` | `{reason?}`: out, and refused if they try to join again |
| `POST /pond/v1/admin/members/{member}/mute` | `{until?}` (ms): they stay, but may not write or be heard (see "Muting") |
| `POST /pond/v1/admin/members/{member}/unmute` | |
| `GET /pond/v1/admin/bans` | Who is banned, by whom, when and why |
| `POST /pond/v1/admin/bans/{member}/unban` | |
| `GET /pond/v1/admin/invites` | The invites that can still be used, each with its link |
| `POST /pond/v1/admin/invites/create` | `{uses?, expires?}` (ms): answers the invite, with its link |
| `POST /pond/v1/admin/invites/{token}/revoke` | It stops working at once |

Rank decides who may act on whom: an operator on administrators and members, an administrator on members only.
Nobody acts on an operator from the app (only `pondctl` can), and nobody on themselves. Each of these goes in
`admin_log` (who, what, when), as do `pondctl`'s, for a record of who did what.

### Muting

An operator or administrator mutes a member for a while (`until`, in ms) or until they are unmuted. A muted member stays,
reads, and sees everything, but a send is refused (`409 muted`, with `until`), and in a voice channel the Pond passes on
nothing they say. Every member hears of it in a `member` event: the member carries `muted: {until}` (`until` null for
until unmuted), or `muted: null`. A mute that runs out ends by itself, with no event: the apps know `until`. The same rank
rules apply as for removing and banning. Mutes are kept in `members.muted_until`, and `pondd` holds who is muted in memory
(read again every two seconds, so voice can ask for every frame).

Bans are by user id. A member who makes a new identity is a new member; banning by anything else (an address, a
device) is not possible, since the Pond knows nothing else of them.

### Protocol

These are additions, so they stay in `/pond/v1`: `protocol` becomes 2, and `features` gains `roles`. Muting came with
protocol 7: a member gains `muted`. Direct messages came with protocol 8 (`dms.md`): a member gains `dms`. A member and `me`
gain `role` (`operator`, `administrator` or `member`), and `/info` gains `offer` (`{id, role}` or null).

### Open questions

- What an operator may do that an administrator may not, setting by setting.
- Whether operators should be able to offer roles from the app, and administrators offer administrator.
- Whether an operator may step down from the app, and whether the last operator may.
- Bans with an end date, and a ban list that `pondctl` can import and export.

## What the database holds

| Table | Columns |
|---|---|
| `settings` | `name`, `value`: the Pond's name and description, its `icon` (a picture's hash), `files.max_size`, and `event_lock` (see "Events") |
| `schema_version` | `version`: which of `db.cpp`'s steps have been run |
| `channels` | `id`, `name`, `topic`, `position`, `created_ms`, `deleted`, `kind` (`text` or `voice`) |
| `members` | `id` (user id), `sign_pub`, `kid`, `chain`, `profile` (JSON), `joined_ms`, `left_ms` |
| `invites` | `token`, `max_uses` (NULL = unlimited), `uses`, `expires_ms` (NULL = never), `created_ms`, `revoked` |
| `messages` | `id`, `channel`, `author`, `text`, `nonce`, `ts_ms`; one index on `(channel, id)`, and a unique one on `(author, nonce)` |
| `events` | `id`, `kind`, `data` (JSON), `ts_ms` |
| `members` (more) | `role`: `operator`, `administrator` or `member`; `muted_until`: NULL, 0 (until unmuted) or when (ms) |
| `role_offers` | `id`, `member`, `role`, `created_ms`; open until answered or 7 days old |
| `bans` | `member`, `by` (a member id, or `pondctl`), `reason`, `ts_ms` |
| `admin_log` | `id`, `by`, `action`, `target`, `detail`, `ts_ms` |
| `events` (more) | `only_for`: a member id, for an event only they receive |
| `files` | `id`, `size`, `chunks` (the hashes, 32 bytes each), `have` (a `0` or `1` per chunk), `whole`, `uploader`, `created_ms` |
| `message_files` | `message`, `position`, `file`, and what the sender said of it: `name`, `mime`, `size`, `w`, `h`, `duration_ms`, `preview` |

Channel names are what a Discord channel name is: 1 to 32 characters of lowercase letters, digits and `-`. Each one
is unique among channels that have not been deleted. A deleted channel keeps its messages in the database, but nothing
can read them.

## `pondctl`

```
pondctl [-c pond.yaml] init <name>              make the key and the database tables, and name the Pond
pondctl name <name> [--description <text>]
pondctl icon <picture file>|off                 the Pond's icon (see "Pictures")
pondctl channel create <name> [--voice] [--topic <text>]   --voice: a voice channel
pondctl channel list
pondctl channel rename <name> <new name>
pondctl channel topic <name> <text>
pondctl channel delete <name>
pondctl invite create [--uses N] [--expires 7d|24h|2026-12-31]
pondctl invite list
pondctl invite revoke <token>
pondctl member list                             with each member's role
pondctl member offer <member> operator|administrator [--yes]   asks first; they accept in their app
pondctl member role <member> member|administrator              take a role away, or lower it
pondctl member remove <member>
pondctl member ban <member> [--reason <text>]
pondctl member unban <user id>
pondctl member bans
pondctl log [N]                                 the last N admin actions, from the app and from pondctl
pondctl files                                   the largest file members may send, and the room files take
pondctl files max-size <size>|off               25M, 1G, 500K...; off stops members sending files
pondctl info                                    the Pond's id, address, certificate fingerprint and counts
```

`invite create` prints the link to give out. `init` also makes a channel called `general`. `<member>` is a user id, or
part of a member's display name that only one member's has.

## `pond.yaml`

```yaml
listen: 0.0.0.0           # where pondd listens
ports:
  chat: 443               # TCP (HTTPS): everything but voice: joining, channels, messages, files, invites
  voice: 443              # UDP: voice and video in voice channels; the chat port's number unless set
address: pond.example.org # what invite links say, host[:chat port]; the port is left off when it is 443
data_dir: /var/lib/pond   # pond.key, cert.pem, key.pem
threads: 0                # threads answering requests; 0 is one per CPU
events_wait_ms: 25000     # how long a request for events is held open (the tests shorten it)
voice:                    # voice channels (voice.md); "voice: false" turns them off
  max_in_channel: 0       # people in one voice channel; 0 is no limit
database:
  host: 127.0.0.1         # "localhost" is the client library's default Unix socket
  port: 3306
  user: pond
  password: secret
  name: pond
  connections: 16
```

Unknown settings are an error, so a typo is not silently ignored.

**Two ports, one number.** A Pond has a **chat port** (TCP: HTTPS, everything a member does except talk) and a **voice
port** (UDP: the sound and video of voice channels). TCP and UDP port numbers are separate, so both can be 443, and are
unless said: someone running a Pond opens one number, 443, for both protocols (on most routers one rule, "TCP/UDP"),
and 443 is the port networks least often block. Give them different numbers only when something else has one: a
reverse proxy on TCP 443 in front of the Pond (it can't carry UDP, so voice goes straight to the Pond), or a proxy using
UDP 443 for HTTP/3. Invite links carry the chat port; a Puddle learns the voice port when it joins a voice channel.

`port` and `voice.port`, the older names for `ports.chat` and `ports.voice`, are still read, and pondd logs a note to
rename them. Before `ports`, the default was 8443; a `pond.yaml` that says nothing keeps 443 now, so one written before
it that left the port out moves to 443. Drogon's MariaDB client cannot be given a socket path:
with `host: localhost` it uses the client library's default socket, and otherwise it connects over TCP.

## Versions

A Pond and a Puddle are built and upgraded apart, so neither may assume the other is the same age. What ties them is
the protocol, not the version of either program.

- The paths say the protocol: `/pond/v1/...`. A change an older Puddle would misread (a field that means something
  else, a route that answers differently) gets new paths, `/pond/v2/...`, and a Pond serves both until its members
  have moved.
- Within v1, things are only added: new fields, new routes, new event kinds. A Puddle ignores fields and event kinds
  it does not know, so an older Puddle keeps working with a newer Pond.
- `/pond/v1/info` (and joining) says `protocol`, a number that goes up with each addition, and `features`, the names
  of optional parts this Pond has (`files`, `roles`, `receipts`, `sealed`, `pictures`, `voice`, `dms`, `dm_files`). A Puddle checks these before using an addition, rather than guessing from
  a `404`, which is also what a refusal looks like. An answer without them is protocol 1 with no features.

A Puddle records what each Pond says on every `/info`, so it notices when a Pond is upgraded.

## Limits

- Requests that fail any check get `404`.
- Bodies are up to 32 KiB, apart from a send (64 KiB), beginning a file (256 KiB), and a file chunk or a picture (1 MiB). A
  profile is up to 16 KiB of JSON, and is stored as received, apart from that limit and being a JSON object.
- Connections are not limited by `pondd` beyond Drogon's default (100,000) and the open-file limit it is given. Each
  member's Puddle holds one open nearly all the time.
- Each member may send 5 messages a second, with bursts of up to 20.
- Each member may send or fetch 20 file chunks a second (about 20 MiB/s), with bursts of 64.
- A file is at most what the admin allows (25 MiB unless changed), and never more than 4 GiB. A message carries up to 10.

## Coroutines

pondd's handlers are C++20 coroutines over drogon (`Task<...>`, `co_await` on the database). **GCC 12 builds some of them
wrong when optimising**: the heap is damaged, and pondd dies of it (`free(): double free`, `free(): invalid pointer`) on
whichever request happens to run that code, never in a debug build. With the image built on Debian 12 it was every join,
then every join with a profile, then every voice channel joined; fixing each in the code only found the next.

So **pondd is built with GCC 13 or later** (or Clang). The image is built on Debian 13 (GCC 14), CI on Ubuntu 24.04
(GCC 13), and `CMakeLists.txt` refuses an optimised build on GCC 12 or older. The code that crashed on GCC 12, built on
Debian 13, joins with a profile and joins voice channels without a fault.

The code still keeps to the rules that worked around it, because they are also simply clearer:

- **One `co_await` per statement**, its result in a named variable, and the text built from those afterwards. Never
  `"..." + co_await f() + "..."`, never `f(co_await g())`, never `co_await f(co_await g())`.
- **No `?:` that makes an object in a `co_await` or `co_return` statement**, in what is returned or in an argument: put it
  in a named variable first (`nullable()` in `pond.cpp`), or write `if`. And no `co_await` inside a `?:`.
- **An awaiter that hands `this` to another thread is a named variable** (`OffLoop` in `voice.cpp`), not a temporary.
- **Coroutines take their arguments by value** (`std::string`, `DbPtr`, `Result`), never `const &`: a reference to a caller's
  temporary has to outlive a suspension. `Signing &` in `Routes::answer` stays a reference: it is an output, and names the
  caller's own variable.

### When pondd crashes anyway

A crash can't be caught and carried on from: by the time glibc says `free(): ...` or a pointer faults, memory pondd relies
on is already wrong, and going on could write wrong data to the database. pondd dies, and Docker or the service manager
starts it again in a second. Errors that can be caught (a database refusing, a bad request, an exception in a handler) are
caught, logged and answered, and pondd carries on.

What it does before dying (`src/crash.h`) is say what it was doing: the signal, every request it was answering at that
moment (`POST /pond/v1/channels/2/voice/join from 172.19.0.1`), and a backtrace with pondd's function names. That is the line
to report.

## Not yet

- Permissions finer than the three roles, such as who may post where.
- Editing, deleting and replying to messages, reactions, mentions, unread counts and typing indicators.
- Messages signed by their authors, so a Pond could not forge one. Today members trust the Pond for what it shows,
  as they trust a Discord server.
- Redis, and several `pondd` processes. The client for Redis is already built in; nothing uses it yet.
