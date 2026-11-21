#pragma once

/* Voice channels (docs/voice.md): members drop into one and talk, and the Pond forwards. Each member's Puddle makes one WebRTC
 * connection to the Pond (it offers, we answer), sends its owner's voice on an Opus track, camera on an H.264 track (`video`)
 * and a shared screen on another (`screen`), and
 * gets back every other voice in the channel over a data channel, `frames` (a 2-byte slot, a 4-byte RTP timestamp, the Opus
 * frame), and the cameras and screens it asked to watch on its video tracks v1 .. v16. The Pond decodes nothing.
 *
 * Who is in which channel lives here, in memory: it is gone when pondd stops, as the connections are. Members hear of it as
 * ordinary events (`voice`, `voice_left`), sent once a connection is up, and in /info. Only built with libdatachannel
 * (POND_HAVE_VOICE); without it there is no Voice, and `features` doesn't say `voice`. */

#include "pond.h"

#include <trantor/net/EventLoop.h>

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>

namespace pond {

constexpr int VOICE_TRACKS = 16;              /* cameras one member can watch at once: v1 .. v16 */
constexpr int VOICE_SLOTS = 65535;            /* members in one channel, at most (a slot is 2 bytes) */
constexpr size_t VOICE_FRAME_MAX = 1500;      /* an Opus frame */
constexpr size_t VOICE_VIDEO_MAX = 512 * 1024; /* an H.264 access unit */

struct VoiceSettings {
    unsigned port = 0;           /* the UDP port every connection shares */
    std::string bind;            /* the address to take it on; "" = every one */
    unsigned max_in_channel = 0; /* 0 = no limit but the slots */
    int connect_ms = 20000;      /* a connection not up this long after joining is let go */
    int lost_ms = 10000;         /* nor one that dropped and hasn't come back in this long */
};

class Voice {
public:
    /* `loop` runs the events Voice sends (through `svc`) and its checks; it must be running (pondd's main loop). */
    Voice(Service &svc, VoiceSettings settings, trantor::EventLoop *loop);
    ~Voice();
    Voice(const Voice &) = delete;
    Voice &operator=(const Voice &) = delete;

    static bool built();

    /* Start the checks (deadlines; members and channels that went), and stop: every connection closed. */
    void start();
    void stop();

    /* `peer_ip` is the address the request came from: the Pond adds it, at the `udp` the Puddle said, as a candidate. */
    Task<Answer> join(std::string member, int64_t channel, std::string sdp, int udp, std::string peer_ip);
    Answer state(const std::string &member, const std::string &session, std::optional<bool> muted, std::optional<bool> deafened,
                 std::optional<bool> camera, std::optional<bool> screen = std::nullopt);
    /* `tracks`: track number (1 .. VOICE_TRACKS) to whose camera goes on it (a member's id), or whose shared screen ("<id>/screen");
     * "" for none */
    Answer watch(const std::string &member, const std::string &session, const std::map<int, std::string> &tracks);
    Answer leave(const std::string &member, const std::string &session);

    /* Who is in which voice channel now, as /info says it, leaving out the channels in `hidden` */
    std::string now_json(const std::set<int64_t> &hidden);

    struct Impl;

private:
    std::shared_ptr<Impl> impl_;
};

} // namespace pond
