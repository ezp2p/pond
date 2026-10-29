# Pond design (first cut)

A **Pond** is a community server, like a Discord server: someone runs it, it has text channels, and people join with
an invite link. It is not a Puddle. Its members are Puddles, and each one connects to it with the same key it uses
everywhere else. The routes are in `api/pond.yaml`.

This first cut does text channels, and files in messages (see "Files"). Permissions and roles, voice, editing and
deleting messages, and moderation beyond removing a member all come later (see "Not yet" at the end).

## What runs

- **`pondd`**: the service. It has no UI and serves HTTPS on one port. It reads `pond.yaml`.
- **`pondctl`**: the admin tool. It works on the same database and data folder as `pondd` and needs the same config.
  It sets up a new Pond, names it, manages channels and invites, and lists or removes members. `pondd` does not
  have to be stopped while it runs.
- **MariaDB** holds everything: the Pond's settings, channels, members, invites, messages and the event log.
- **Redis** is not used yet. It comes in when one Pond runs as several `pondd` processes and they need to tell each
  other about new events, or share rate limits. Until then the events table is the only way news travels.

The Pond's own files live in `data_dir`. They are its key (`pond.key`, mode 0600), the TLS certificate and key
(`cert.pem`, `key.pem`), and the files members send (`files/`, see "Files"). The certificate is made self-signed on first start, as a Puddle does, unless the operator has
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

## Invite links

```
pond://pond.example.org?invite=k3v8y2m4q7c9x5t1w6n0r3z8ab
pond://203.0.113.7:8443?invite=...
```

- The address part follows the same rules as a Puddle address: a host or IP, the port defaulting to 443, and no path.
- The token is 26 characters of base32 (16 random bytes), the same form as a Puddle invite.
- `pondctl invite create` makes an invite. It can have a number of uses (`--uses 10`), an expiry (`--expires 7d`, or a
  date), both, or neither, in which case it lasts until revoked.
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

The Puddle records the certificate fingerprint it saw, as it does on first contact with a Puddle, and compares it on
every later call.

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
- `pond`: the Pond's name or description changed, or the admin changed what files may be sent.

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

## What the database holds

| Table | Columns |
|---|---|
| `settings` | `name`, `value`: the Pond's name and description, `files.max_size`, and `event_lock` (see "Events") |
| `schema_version` | `version`: which of `db.cpp`'s steps have been run |
| `channels` | `id`, `name`, `topic`, `position`, `created_ms`, `deleted` |
| `members` | `id` (user id), `sign_pub`, `kid`, `chain`, `profile` (JSON), `joined_ms`, `left_ms` |
| `invites` | `token`, `max_uses` (NULL = unlimited), `uses`, `expires_ms` (NULL = never), `created_ms`, `revoked` |
| `messages` | `id`, `channel`, `author`, `text`, `nonce`, `ts_ms`; one index on `(channel, id)`, and a unique one on `(author, nonce)` |
| `events` | `id`, `kind`, `data` (JSON), `ts_ms` |
| `files` | `id`, `size`, `chunks` (the hashes, 32 bytes each), `have` (a `0` or `1` per chunk), `whole`, `uploader`, `created_ms` |
| `message_files` | `message`, `position`, `file`, and what the sender said of it: `name`, `mime`, `size`, `w`, `h`, `duration_ms`, `preview` |

Channel names are what a Discord channel name is: 1 to 32 characters of lowercase letters, digits and `-`. Each one
is unique among channels that have not been deleted. A deleted channel keeps its messages in the database, but nothing
can read them.

## `pondctl`

```
pondctl [-c pond.yaml] init <name>              make the key and the database tables, and name the Pond
pondctl name <name> [--description <text>]
pondctl channel create <name> [--topic <text>]
pondctl channel list
pondctl channel rename <name> <new name>
pondctl channel topic <name> <text>
pondctl channel delete <name>
pondctl invite create [--uses N] [--expires 7d|24h|2026-12-31]
pondctl invite list
pondctl invite revoke <token>
pondctl member list
pondctl member remove <user id>
pondctl files                                   the largest file members may send, and the room files take
pondctl files max-size <size>|off               25M, 1G, 500K...; off stops members sending files
pondctl info                                    the Pond's id, address, certificate fingerprint and counts
```

`invite create` prints the link to give out. `init` also makes a channel called `general`.

## `pond.yaml`

```yaml
listen: 0.0.0.0           # where pondd listens
port: 8443
address: pond.example.org:8443   # what invite links say; port left off if 443
data_dir: /var/lib/pond   # pond.key, cert.pem, key.pem
threads: 0                # threads answering requests; 0 is one per CPU
events_wait_ms: 25000     # how long a request for events is held open (the tests shorten it)
database:
  host: 127.0.0.1         # "localhost" is the client library's default Unix socket
  port: 3306
  user: pond
  password: secret
  name: pond
  connections: 16
```

Unknown settings are an error, so a typo is not silently ignored. Drogon's MariaDB client cannot be given a socket path:
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
  of optional parts this Pond has (`files`). A Puddle checks these before using an addition, rather than guessing from
  a `404`, which is also what a refusal looks like. An answer without them is protocol 1 with no features.

A Puddle records what each Pond says on every `/info`, so it notices when a Pond is upgraded.

## Limits

- Requests that fail any check get `404`.
- Bodies are up to 32 KiB, apart from a send (64 KiB), beginning a file (256 KiB) and a file chunk (1 MiB). A
  profile is up to 16 KiB of JSON, and is stored as received, apart from that limit and being a JSON object.
- Connections are not limited by `pondd` beyond Drogon's default (100,000) and the open-file limit it is given. Each
  member's Puddle holds one open nearly all the time.
- Each member may send 5 messages a second, with bursts of up to 20.
- Each member may send or fetch 20 file chunks a second (about 20 MiB/s), with bursts of 64.
- A file is at most what the admin allows (25 MiB unless changed), and never more than 4 GiB. A message carries up to 10.

## Not yet

- Roles and permissions, including who may post where, and admin from inside the app.
- Editing, deleting and replying to messages, reactions, mentions, unread counts and typing indicators.
- Bans. Removing a member does not stop them joining again with a valid invite.
- Messages signed by their authors, so a Pond could not forge one. Today members trust the Pond for what it shows,
  as they trust a Discord server.
- A Pond changing its TLS certificate. The Puddle pins the one it saw at join, so a new certificate (for example a
  renewed Let's Encrypt one) makes the Pond unreachable until the member joins again. A signed announcement of the
  new certificate, like a Puddle's address record, will fix this.
- Redis, and several `pondd` processes. The client for Redis is already built in; nothing uses it yet.
