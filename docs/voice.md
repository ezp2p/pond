# Voice channels (draft)

The Pond's side is built (step 1 below, in `src/voice.*`); the Puddle's isn't yet. A voice channel is a channel members drop into to talk, as on Discord: you join it and sit
there until you leave, and everyone in the Pond can see who is in it. It is not a call: nobody is rung, nothing alerts
anyone, and nothing is noted in a history. Members can turn their camera on in it too ("Video").

This page covers the Pond's side and the Puddle's, since neither works without the other; once settled, the wire goes
into the Puddle Protocol as an addition to spec/05.

It builds on the calls Puddles already make to each other (Puddle's `docs/calls.md`): WebRTC through libdatachannel,
Opus and H.264, no STUN or TURN, and a Puddle that carries its owner's sound and pictures so that a phone only ever
talks to its own Puddle. What is different is who connects to whom.

## Why not the group call mesh

A group call connects every pair of the people in it, so each Puddle sends its owner's voice once per other person.
That is fine for 8. A Pond channel may have 50 people in it, and a Puddle on a home connection can't send 49 copies of
anything. So in a Pond, **the Pond forwards**: each Puddle sends its owner's voice once, to the Pond, and the Pond
sends each Puddle what the others say. This is what's called an SFU (a selective forwarding unit). The Pond decodes
nothing and mixes nothing; it passes frames on, as a Puddle does.

    window ──(in process)── Puddle A ══ WebRTC ══╗
                                                 ║
    phone ══ leg ══ Puddle B ══════ WebRTC ══════╬══ Pond
                                                 ║
    window ──(in process)── Puddle C ══ WebRTC ══╝

It also fits how Ponds already work: **the Puddle always connects to the Pond**, never the other way round, and the Pond
is the one with a public address. So a Puddle behind NAT can join voice, which it can't do for a call with a friend
today. The Pond needs no STUN server to learn where a Puddle is: it learns that from the Puddle's first ICE check,
which arrives from the address the NAT gave it (a peer-reflexive candidate).

**The Pond hears and sees everything in its voice channels**, as it reads everything in its text channels. Members
trust a Pond with voice as they do with text, and as people trust a Discord server. Encrypting so that only the
members can hear (SFrame, with a key the members share) can come later without changing the rest ("Later").

## A voice channel

A channel gains `kind`: `text` (every channel today) or `voice`. Administrators and operators make them, as they make
text channels (`POST /pond/v1/admin/channels/create {name, topic?, kind?}`, and `pondctl channel create --voice`). A
channel's kind doesn't change afterwards.

A voice channel also takes text, like any channel: a place to paste a link while talking. That keeps an older Puddle,
which knows nothing of `kind`, working: it shows the channel as a text channel, and what it sends there is a message.

## Who is in it

Being in a voice channel is shown to every member, so the Pond tells everyone:

- `voice`: someone came into a voice channel, or changed how they are in it:
  `{channel, member, slot, muted, deafened, camera, screen}`.
- `voice_left`: someone left one: `{channel, member}`.

`/info` gains `voice`: for each voice channel with anyone in it, who is (the same fields). So a Puddle that missed events
(a reset) still knows. These are ordinary events, from the events table; they don't need to be quick, only right.

`slot` is the number the Pond gave that member in that channel: the smallest free, from 1. Each frame the Pond forwards
carries the slot, not the member's 52-character id, so it stays a few bytes. A slot is held while the member is in the
channel, and freed when they leave.

A member is in at most one voice channel of a Pond at a time. Joining another leaves the first.

## Joining, and the connection

The Puddle makes the connection, signed like every request a member makes, and answered with the Pond's receipt:

| Route | Body | Answer |
|---|---|---|
| `POST /pond/v1/channels/{channel}/voice/join` | `{sdp, udp}` | `200 {session, sdp, udp, slot}`; `404` not a voice channel |
| `POST /pond/v1/voice/{session}/state` | `{muted, deafened, camera, screen}` | `204` |
| `POST /pond/v1/voice/{session}/watch` | `{tracks: {"v1": member, "v2": "<member>/screen", ...}}` | `204`: whose camera or screen to send on which track ("Video") |
| `POST /pond/v1/voice/{session}/leave` | | `204` |

- **The offer** is the Puddle's, complete with its candidates (nothing trickles), as with a phone's leg. It has an Opus
  track `audio`, an H.264 track `video` (the camera) and another, `screen` (a shared screen), all sent by the Puddle; video
  tracks `v1` to `v16` that only come in; and a
  data channel `frames` (unordered, never sent again) for the voices that come back. `udp` is the port the Puddle's calls
  use, so the Pond can add the Puddle's own addresses; behind NAT those won't answer, and the peer-reflexive one will.
- **The answer** names the Pond's UDP port for voice (`ports.voice` in `pond.yaml`, the chat port's number unless set),
  and the Pond's advertised address is a candidate at that port. Every connection to the Pond shares that one port, so
  a Pond operator opens one UDP port beside the HTTPS one.
- **Bound to who they are**: the offer names the Puddle's DTLS fingerprint and travels in a request signed by the member;
  the answer names the Pond's and comes with the Pond's receipt. Nobody in the middle can put their own in.
- **Going away**: the Pond lets a member go (`voice_left`) when they leave, when the connection doesn't come up within
  20 seconds of joining, or when it drops (or closes, or fails) and isn't back within 10. Being removed or banned ends it
  too, and so does the channel being deleted.

Asking to join again while in, with a new offer, replaces the connection: for a Puddle whose address changed, or that
restarted. In the same channel it keeps its slot, and nobody is told anything, as long as it comes within those 10
seconds.

## Voice

Each Puddle sends its owner's voice as an RTP track, as in a call: Opus, 48 kHz, mono, 20 ms frames, with in-band FEC.
It also uses Opus's DTX, so **someone who isn't talking sends almost nothing**: a frame every 400 ms instead of fifty a
second.

The Pond sends each Puddle **every frame the others send**, over `frames`, one message per frame: the slot (2 bytes,
big-endian), the frame's RTP timestamp (4 bytes, big-endian), then the Opus frame. The device keeps a jitter buffer and
decoder for each voice and plays them mixed, as it does for a phone in a group call (Puddle's `docs/calls.md`, "Group
calls on a phone"; that format has a 1-byte slot, and this one 2, so a big channel never runs out). Who is talking is
lit from the frames arriving. Why a data channel and not a track per speaker: people come and go all the time in a
voice channel, and new tracks would mean renegotiating each time. A slot in each message costs nothing.

**No cap.** Because silence is nearly free with DTX, what a channel costs is how many talk at once, not how many are in
it. With 50 in a channel and 3 talking, the Pond gets about 100 kbit/s of voice and sends each member about 100 kbit/s.
If 20 talk at once it is 640 kbit/s to each, which a home connection takes, and the talk is chaos anyway. So the Pond
takes everyone and forwards everything; there is no limit on people in a channel or voices forwarded. An operator who
wants one can set `voice.max_in_channel`; unset, there is none.

A member who is muted sends nothing (the device stops); the Pond also drops what a member sends while the Pond knows them
as muted. A member who is deafened is sent nothing.

### Talking: voice activation or push to talk

How the device decides when its owner is talking is a setting of the owner's, on the device, not the Pond's:

- **Voice activation** (the default): the microphone is sent while it hears speech. The device gates on the level after
  echo cancelling and noise suppression, with a threshold the owner can move and a short hang time (300 ms) so word
  endings aren't cut off. Between, it sends nothing (DTX's own silence frames).
- **Push to talk**: the microphone is sent only while a key is held (the window; a shortcut the owner chooses, which works
  while the window isn't in front where the system allows it) or a button is pressed (a phone).

Either way the Pond sees only frames or their absence; it doesn't know which the owner uses.

## Video

Anyone in a voice channel can turn their camera on (`camera` in the state, so everyone sees whose is on). The camera is
off when joining. It is H.264 as in a call (Puddle's `docs/calls.md`, "Video": Constrained Baseline, 640×480, 15 frames
a second, about 600 kbit/s, a key frame every 2 seconds), sent on the `video` track, and the Pond passes whole frames on
as a Puddle does.

Video can't come back over `frames` (a key frame is dozens of packets, and losing one would lose it all), and adding a
track for each camera would mean renegotiating each time one turns on. So each connection has **16 video tracks that only
come in**, `v1` to `v16`, and the Puddle says whose camera it wants on which (`watch`). The Pond sends each camera only to
those watching it. That is a limit on how many one member watches at once, not on how many cameras are on: 16 is what a
window can show at a useful size, and 16 cameras is about 10 Mbit/s coming in.

- The window shows the cameras that are on as tiles above the channel, and watches all of them up to 16. If more are on,
  it watches whoever spoke most recently, and the rest show as names until one speaks.
- A phone watches fewer (4 unless the owner changes it), and none on a mobile network unless asked.
- When a member starts watching a camera, the Pond holds that track back until the camera's next key frame, so nobody is
  shown a picture smeared from missing the start. At most 2 seconds.

**Sharing a screen** is a second video track from the Puddle, `screen`, sent while `screen` is true in its state (so everyone
sees who is sharing). Several members may share at once. It is watched the same way, on any of v1 .. v16, as
`"<member>/screen"` instead of the member's id; a watcher chooses which of the screens shared to look at. It may be larger
than a camera's picture and carry more (up to 4 Mbit/s, apart from the camera's 2), so text on it stays sharp.

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

- **It isn't a call.** Being in a voice channel never rings, notifies, or adds a note to a chat, and it isn't in the
  Puddle's calls (`/api/v1/calls`). It has routes of its own: `POST /api/v1/ponds/{pond}/channels/{channel}/voice/join`,
  `POST /api/v1/ponds/{pond}/voice/leave`, `/voice/state {muted, deafened, camera}` and `/voice/watch`, and
  `GET /api/v1/ponds/{pond}` says who is in each voice channel. Underneath, it uses what calls use on the device: the
  microphone and speaker, the echo canceller, a jitter buffer and decoder for each voice, the camera and H.264, and a
  phone's leg.
- **Already in a call.** The owner talks in one place at a time: a call or a voice channel. While they are in a voice
  channel, a friend who calls them is answered `busy`, and the friend sees "*Ann* is already in a call", just as when
  they are in a call. Starting a call, or joining a voice channel, while in either is refused the same way ("You're
  already in a call"), so the owner leaves one first.
- **In the window**, a voice channel shows a speaker icon in the channel list, with who is in it underneath. Clicking it
  joins; there is nothing to answer. While in it, a bar at the bottom of the channel list says which channel, with
  mute, deafen, camera and leave. Who is talking is lit. Settings gain voice activation or push to talk, its key, and the
  threshold.
- **On a phone**, the leg is the phone's own leg to its Puddle, as for a group call: the phone's voice and camera go up
  its tracks, the Puddle passes the voices on over the leg's `frames`, and the cameras the phone watches on its incoming
  video tracks.
- A Puddle checks for `voice` in the Pond's `features` before offering any of it, and shows a voice channel of a Pond
  that doesn't say it (which can't happen today) as a text channel.

## Limits

- None on people in a channel, or on voices forwarded, unless the operator sets `voice.max_in_channel`.
- 16 cameras watched at once by one member (the tracks).
- The Pond drops frames from a member beyond 60 voice frames and 30 video frames a second, and video beyond 2 Mbit/s,
  so a broken or hostile Puddle can't flood the others.
- `pond.yaml` gains `voice: {port, max_in_channel}`; `voice: false` turns voice off, and then no channel may be a voice
  channel (`features` doesn't say `voice`).

## Built in Pond

`src/voice.*`, with libdatachannel: the system's where there is one (Arch, Homebrew), otherwise fetched by CMake at a fixed
release as Drogon is (Debian has no package). Without it (`-DPOND_VOICE=OFF`) Pond builds with no voice, and `features`
doesn't say `voice`. Connections run on libdatachannel's own threads, beside Drogon's; setting one up waits on them, so it
happens on a thread of its own rather than one answering requests. Joining and leaving go through the database (the
`voice` and `voice_left` events, sent one after another in order), while the frames never touch it.

Who is in which channel is kept only in `pondd`'s memory, as the connections are, so a restart empties every voice
channel (the members' Puddles join again). `pondctl` can't see it; it makes voice channels (`channel create --voice`),
and a member it removes or a channel it deletes is noticed by `pondd` within a second or two, and let go.

`tests/voice_test.cpp` connects members with libdatachannel as a Puddle does, and checks: only a voice channel, only a
member; who is in it, in events and `/info`; every voice to everyone else with the sender's slot, and nobody hearing
themselves; muted and deafened; a camera only to whoever watches it, only while it is on, and from a key frame on; text in a
voice channel; leaving, and joining again keeping the slot unannounced; being removed by `pondctl`, and the channel deleted.

Several `pondd` processes (with Redis, later) would each have to hold a whole voice channel; that's for when there is
more than one.

## Steps

1. **Pond: voice channels and forwarding** (built). The channel kind, joining and leaving, the events, every voice
   forwarded over `frames`; and the Pond's half of video (cameras and `watch`), since its tracks are in the offer from
   the start.
2. **Puddle: joining from the window.** The voice routes, the channel list, the bar, who is talking, voice activation and
   push to talk, and "already in a call" both ways.
3. **On a phone,** over its leg.
4. **Video in the app:** the camera, choosing whom to watch, the tiles.
5. **Moderation:** disconnect and mute by admins.

## Moderation

Built 2026-11-23 (protocol 13; docs/design.md, "Voice moderation" and "Permissions"). Whoever may there mutes someone in
the channel for everyone (`POST /pond/v1/voice/members/{member}/mute {muted}`: the Pond forwards nothing of theirs) or
disconnects them (`/disconnect`: their session is dropped, and `voice_left` says `by`; their Puddle doesn't come back by
itself). The channel's permissions apply too: without `connect` nobody joins it, without `speak` the Pond forwards none of
their voice, without `video` none of their camera or screen. The Pond looks at them again every second or two, so a role
or a channel changed applies to those already in it, and someone's state says `server_muted`, `speak` and `video`.

## Later

- **Encryption only members can undo** (SFrame): each member encrypts their frames with a key the channel's members
  share, and the Pond forwards what it can't read. Nothing above changes but the frames' contents.
- **Asking for a key frame** (RTCP PLI) and resending lost packets (NACK), as for calls.
- **A smaller picture for small tiles** (simulcast: each camera sent at two sizes, and the Pond sends each watcher the one
  that fits), and bitrate that follows each member's connection.
