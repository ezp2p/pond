# Voice channels (draft)

A design draft, not built. A voice channel is a channel members drop into to talk, as on Discord: nobody rings anyone,
you are in it until you leave, and everyone in the Pond can see who is in it. This page covers the Pond's side and the
Puddle's, since neither works without the other; once settled, the wire goes into the Puddle Protocol as an addition to
spec/05.

It builds on the calls Puddles already make to each other (Puddle's `docs/calls.md`): WebRTC through libdatachannel,
Opus, no STUN or TURN, and a Puddle that carries its owner's sound so that a phone only ever talks to its own Puddle.
What is different is who connects to whom.

## Why not the group call mesh

A group call connects every pair of the people in it, so each Puddle sends its owner's voice once per other person.
That is fine for 8. A Pond channel may have 30 people in it, and a Puddle on a home connection can't send 29 copies of
anything. So in a Pond, **the Pond forwards**: each Puddle sends its owner's voice once, to the Pond, and the Pond
sends each Puddle what the others say. This is what's called an SFU (a selective forwarding unit). The Pond decodes
nothing and mixes nothing; it passes Opus frames on, as a Puddle does.

    window ──(in process)── Puddle A ══ WebRTC ══╗
                                                 ║
    phone ══ leg ══ Puddle B ══════ WebRTC ══════╬══ Pond
                                                 ║
    window ──(in process)── Puddle C ══ WebRTC ══╝

It also fits how Ponds already work: **the Puddle always connects to the Pond**, never the other way round, and the Pond
is the one with a public address. So a Puddle behind NAT can join voice, which it can't do for a call with a friend
today. The Pond needs no STUN server to learn where a Puddle is: it learns that from the Puddle's first ICE check,
which arrives from the address the NAT gave it (a peer-reflexive candidate).

What it costs: **the Pond hears everything said in its voice channels**, as it reads everything in its text channels.
Members trust a Pond with voice as they do with text, and as people trust a Discord server. Encrypting each voice so
only the members can hear it (SFrame, with a key the members share) is a later step ("Later").

## A voice channel

A channel gains `kind`: `text` (every channel today) or `voice`. An administrator or operator chooses it when making
the channel (`POST /pond/v1/admin/channels/create {name, topic?, kind?}`, and `pondctl channel create --voice`). A
channel's kind doesn't change afterwards.

A voice channel also takes text, like any channel: a place to paste a link while talking. That keeps an older Puddle,
which knows nothing of `kind`, working: it shows the channel as a text channel, and what it sends there is a message.

## Who is in it

Being in a voice channel is shown to every member, so the Pond tells everyone:

- `voice`: someone came into a voice channel, or changed how they are in it: `{channel, member, slot, muted, deafened}`.
- `voice_left`: someone left one: `{channel, member}`.

`/info` gains `voice`: for each voice channel with anyone in it, who is (the same fields). So a Puddle that missed events
(a reset) still knows. These are ordinary events, from the events table; they don't need to be quick, only right.

`slot` is the number the Pond gave that member in that channel: the smallest free from 1 to 255. Each frame the Pond
forwards carries the slot, not the member's 52-character id, so it stays a few bytes. A slot is held while the member is
in the channel, and freed when they leave.

A member is in at most one voice channel of a Pond at a time. Joining another leaves the first.

## Joining, and the connection

The Puddle makes the connection, signed like every request a member makes, and answered with the Pond's receipt:

| Route | Body | Answer |
|---|---|---|
| `POST /pond/v1/channels/{channel}/voice/join` | `{sdp, udp}` | `200 {session, sdp, udp, slot}`; `404` not a voice channel; `409 full` |
| `POST /pond/v1/voice/{session}/state` | `{muted, deafened}` | `204` |
| `POST /pond/v1/voice/{session}/leave` | | `204` |

- **The offer** is the Puddle's, complete with its candidates (nothing trickles), as with a phone's leg. It has one
  Opus track, `sendonly` from the Puddle, and a data channel `frames` (unordered, never sent again) for what comes back.
  `udp` is the port the Puddle's calls use, so the Pond can add the Puddle's own addresses; behind NAT those won't
  answer, and the peer-reflexive one will.
- **The answer** names the Pond's UDP port for voice (`voice.port` in `pond.yaml`, the HTTPS port's number unless set),
  and the Pond's advertised address is a candidate at that port. Every connection to the Pond shares that one port, so
  a Pond operator opens one UDP port beside the HTTPS one.
- **Bound to who they are**: the offer names the Puddle's DTLS fingerprint and travels in a request signed by the member;
  the answer names the Pond's and comes with the Pond's receipt. Nobody in the middle can put their own in.
- **Going away**: the Pond lets a member go (`voice_left`) when they leave, when the connection doesn't come up within
  20 seconds of joining, or when it drops for 10. Being removed or banned ends it too.

Asking to join again while in, with a new offer, replaces the connection: for a Puddle whose address changed, or that
restarted.

## What the Pond forwards

Each Puddle sends its owner's voice as an RTP track, as in a call: Opus, 48 kHz, mono, 20 ms frames, with in-band FEC
and DTX (so silence costs almost nothing). Each frame also carries how loud it is (the RTP header extension for audio
level, RFC 6464), set by the device, so the Pond can tell who is talking without decoding.

The Pond sends each Puddle the others' frames over `frames`, one message per frame: the slot (1 byte), the frame's RTP
timestamp (4 bytes, big-endian), then the Opus frame. That's exactly what a Puddle already sends a phone in a group call
(Puddle's `docs/calls.md`, "Group calls on a phone"), so the same code takes it apart and the same mixer plays it: a
jitter buffer and decoder for each voice. Why a data channel and not a track per speaker: people come and go all the
time in a voice channel, and new tracks would mean renegotiating each time. A slot in each message costs nothing.

**Only who is talking.** The Pond forwards the frames of the members who are talking, at most 6 at once
(`voice.speakers`): those whose audio level was highest over the last half second, with someone already being forwarded
kept a little longer than a newcomer so voices don't flicker in and out. With 30 in a channel the Pond sends each one at
most 6 voices, about 200 kbit/s, and gets 30 at 32 kbit/s; a few dozen people is light work.

A member who is muted sends nothing (the device stops, as in a call); the Pond also drops what a muted member sends.
A member who is deafened is sent nothing.

## Limits

- `voice.max_in_channel` people in one voice channel, 25 unless set, and never more than 255 (the slots).
- `voice.speakers` voices forwarded at once, 6 unless set.
- The Pond drops frames from a member beyond 60 a second, so a broken or hostile Puddle can't flood the others.
- `pond.yaml` gains `voice: {port, max_in_channel, speakers}`; `voice: false` turns voice off, and then no channel may
  be a voice channel (`features` doesn't say `voice`).

## Moderation

What administrators may do to members applies here: removing or banning someone takes them out of voice too. Added
for voice:

| Route | Does |
|---|---|
| `POST /pond/v1/admin/voice/{member}/disconnect` | Out of the voice channel; they may come back |
| `POST /pond/v1/admin/voice/{member}/mute` | `{muted}`: the Pond forwards nothing of theirs until unmuted by an admin |

Rank decides who may act on whom, as for the other admin routes. A member muted by an admin is shown so (`voice` gains
`muted_by_admin`), and can't unmute themselves.

## The Puddle's side

The Puddle carries the connection, as it carries everything to a Pond, so the window and a phone never connect to a
Pond themselves.

- **To the owner, it's a call.** A voice channel shows in the Puddle's calls (`/api/v1/calls`) with `pond` and
  `channel` set instead of `user` or `group`, and `members` from the Pond's `voice` events. So the call bar, mute, the
  speaker and microphone, a phone's leg and the jitter buffers all work as they do for a group call. Joining is
  `POST /api/v1/ponds/{pond}/channels/{channel}/voice/join`; leaving is ending the call.
- **One at a time**, as now: in a voice channel, a friend's ring is answered `busy`, and the other way round. (See
  "Open questions".)
- **In the window**, a voice channel shows a speaker icon in the channel list, with who is in it underneath. Clicking it
  joins. Who is talking is lit, from the frames arriving.
- **On a phone**, the leg is the phone's own leg to its Puddle, as for a group call: the phone's voice goes up its track,
  and the Puddle passes the Pond's `frames` on over the leg's `frames`, with its own slot numbers.
- A Puddle checks for `voice` in the Pond's `features` before offering any of it, and shows a voice channel of a Pond
  that doesn't say it (which can't happen today) as a text channel.

## Building it in Pond

Pond gains libdatachannel, fetched by CMake at a fixed release as Drogon is. It runs on libdatachannel's own threads,
beside Drogon's; joining and leaving go through the database (the `voice` events), while the frames never touch it.
`pondctl` gains `channel create --voice` and `voice list` (who is in which channel now).

Several `pondd` processes (with Redis, later) would each have to hold a whole voice channel; that's for when there is
more than one.

## Steps

1. **Pond: voice channels and forwarding.** The channel kind, joining and leaving, the events, forwarding over `frames`
   with the loudest kept. Tested with Puddle's own call code standing in for members.
2. **Puddle: joining from the window.** The call with `pond` and `channel`, the channel list, who is talking.
3. **On a phone,** over its leg.
4. **Moderation:** disconnect and mute by admins.
5. **Later:** video and screen sharing in a voice channel; encryption only members can undo (SFrame).

## Open questions

- **One call at a time.** Should a friend's call ring while in a voice channel, and pause or leave it if answered, as
  many apps do? This draft says `busy`, as group calls are today.
- **Who may make voice channels**: administrators only, as with text channels, or a setting.
- **Push to talk**, and a setting a channel's admins choose (everyone may talk, or only those let in, like a stage).
- **Video** here would need more than the speakers rule: who is seen, and at what size, for 25 people.
- Whether **the Pond hearing everything** is acceptable to start with, or SFrame should come in step 1.
