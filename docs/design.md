# Pond design (first cut)

A **Pond** is a community server, like a Discord server: someone runs it, it has text channels, and people join with
an invite link. It is not a Puddle. Its members are Puddles, and each one connects to it with the same key it uses
everywhere else. The routes are in `api/pond.yaml`.

This first cut does text channels only. Permissions and roles, voice, attachments, editing and deleting messages,
and moderation beyond removing a member all come later (see "Not yet" at the end).

## What runs

- **`pondd`**: the service. It has no UI and serves HTTPS on one port. It reads `pond.yaml`.
- **`pondctl`**: the admin tool. It works on the same database and data folder as `pondd` and needs the same config.
  It sets up a new Pond, names it, manages channels and invites, and lists or removes members. `pondd` does not
  have to be stopped while it runs.
- **MariaDB** holds everything: the Pond's settings, channels, members, invites, messages and the event log.
- **Redis** is not used yet. It comes in when one Pond runs as several `pondd` processes and they need to tell each
  other about new events, or share rate limits. Until then the events table is the only way news travels.

The Pond's own files live in `data_dir`. They are its key (`pond.key`, mode 0600), and the TLS certificate and key
(`cert.pem`, `key.pem`). The certificate is made self-signed on first start, as a Puddle does, unless the operator has
put their own there.

The code shares the Puddle's lower layers: request signatures, key chains, JSON, the HTTP parser and TLS. CMake builds
them from the Puddle source pinned as the submodule `deps/puddle`, so each Pond commit builds against the Puddle commit
it was made with, whatever the Puddle's own branches are doing. `scripts/build.sh --local` builds against a working
checkout at `../puddle` instead, for changing both at once; the pin is then moved to the Puddle commit that was merged.
This is only about building. A running Pond and a running Puddle depend on each other only through the protocol (see
"Versions").

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

Message text is 1 to 4000 characters, with surrounding whitespace removed. `nonce` is up to 32 characters chosen by
the sender. A second send with the same nonce from the same member answers with the first message and posts nothing,
so a Puddle that lost an answer can safely send again.

## Events

Every change the members need to hear about is a row in the `events` table, with an increasing number. The kinds are:

- `message`: a new message, which is the message itself.
- `channel`: a channel was made or changed, which is the channel itself.
- `channel_deleted`: a channel was deleted (`{id}`).
- `member`: someone joined or changed their profile, which is the member.
- `member_left`: someone left or was removed (`{id}`).
- `pond`: the Pond's name or description changed.

A member's Puddle holds one request open on `/pond/v1/events?after=<cursor>` at a time. The Pond answers as soon as
there is anything after the cursor, up to 200 events at a time, or with an empty list after 25 seconds. The Puddle then
asks again from the new cursor. This works through a Levee and from behind NAT, because the Puddle always makes the
connection.

Events are kept for 7 days. If a cursor is older than the oldest event kept, the answer is `{"reset": true}`, and the
Puddle fetches `/info` and recent history again instead of replaying.

Inside `pondd`, one thread checks the newest event number twice a second and wakes the waiting requests when it
changes. That way a change made by `pondctl`, or by another `pondd` later, is seen without anyone telling it.

## What the database holds

| Table | Columns |
|---|---|
| `settings` | `name`, `value`: the Pond's name and description, and the schema version |
| `channels` | `id`, `name`, `topic`, `position`, `created_ms`, `deleted` |
| `members` | `id` (user id), `sign_pub`, `kid`, `chain`, `profile` (JSON), `joined_ms`, `left_ms` |
| `invites` | `token`, `max_uses` (NULL = unlimited), `uses`, `expires_ms` (NULL = never), `created_ms`, `revoked` |
| `messages` | `id`, `channel`, `author`, `text`, `nonce`, `ts_ms`; one index on `(channel, id)`, and a unique one on `(author, nonce)` |
| `events` | `id`, `kind`, `data` (JSON), `ts_ms` |

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
pondctl info                                    the Pond's id, address, certificate fingerprint and counts
```

`invite create` prints the link to give out. `init` also makes a channel called `general`.

## `pond.yaml`

```yaml
listen: 0.0.0.0           # where pondd listens
port: 8443
address: pond.example.org:8443   # what invite links say; port left off if 443
data_dir: /var/lib/pond   # pond.key, cert.pem, key.pem
database:
  host: 127.0.0.1         # or socket: /path/to/mysql.sock
  port: 3306
  user: pond
  password: secret
  name: pond
```

## Versions

A Pond and a Puddle are built and upgraded apart, so neither may assume the other is the same age. What ties them is
the protocol, not the version of either program.

- The paths say the protocol: `/pond/v1/...`. A change an older Puddle would misread (a field that means something
  else, a route that answers differently) gets new paths, `/pond/v2/...`, and a Pond serves both until its members
  have moved.
- Within v1, things are only added: new fields, new routes, new event kinds. A Puddle ignores fields and event kinds
  it does not know, so an older Puddle keeps working with a newer Pond.
- `/pond/v1/info` (and joining) says `protocol`, a number that goes up with each addition, and `features`, the names
  of optional parts this Pond has (none yet). A Puddle checks these before using an addition, rather than guessing from
  a `404`, which is also what a refusal looks like. An answer without them is protocol 1 with no features.

A Puddle records what each Pond says on every `/info`, so it notices when a Pond is upgraded.

## Limits

- Requests that fail any check get `404`.
- Bodies are up to 32 KiB. A profile is up to 16 KiB of JSON, and is stored as received, apart from that limit and
  being a JSON object.
- At most 2000 connections are held at once, each with a thread. That is the main thing that stops one `pondd`
  scaling far, and it is what Redis and several processes, or an event loop, will replace.
- Each member may send 5 messages a second, with bursts of up to 20.

## Not yet

- Roles and permissions, including who may post where, and admin from inside the app.
- Attachments and images. The Puddle's cache of recent images will come with these.
- Editing, deleting and replying to messages, reactions, mentions, unread counts and typing indicators.
- Bans. Removing a member does not stop them joining again with a valid invite.
- Messages signed by their authors, so a Pond could not forge one. Today members trust the Pond for what it shows,
  as they trust a Discord server.
- A Pond changing its TLS certificate. The Puddle pins the one it saw at join, so a new certificate (for example a
  renewed Let's Encrypt one) makes the Pond unreachable until the member joins again. A signed announcement of the
  new certificate, like a Puddle's address record, will fix this.
- Redis, several `pondd` processes, and an event loop instead of a thread per connection.
