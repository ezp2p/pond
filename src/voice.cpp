#include "voice.h"

#include "encoding.h"
#include "json.h"

#include <trantor/utils/Logger.h>

#ifdef POND_HAVE_VOICE
#include <rtc/rtc.hpp>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace pond {

#ifndef POND_HAVE_VOICE

/* Built without libdatachannel: no voice. pondd makes no Voice then; these are never reached. */
struct Voice::Impl {};
Voice::Voice(Service &, VoiceSettings, trantor::EventLoop *) {}
Voice::~Voice() = default;
bool Voice::built() { return false; }
void Voice::start() {}
void Voice::stop() {}
Task<Answer> Voice::join(std::string, int64_t, std::string, int, std::string) { co_return fail(404, "no_channel"); }
Answer Voice::state(const std::string &, const std::string &, std::optional<bool>, std::optional<bool>, std::optional<bool>, std::optional<bool>)
{
    return fail(404, "no_session");
}
Answer Voice::watch(const std::string &, const std::string &, const std::map<int, std::string> &) { return fail(404, "no_session"); }
Answer Voice::leave(const std::string &, const std::string &) { return fail(404, "no_session"); }
std::string Voice::now_json() { return "[]"; }

#else

#define OPUS_PT 111
#define H264_PT 96

namespace {

uint32_t random32()
{
    unsigned char b[4];
    random_bytes(b, 4);
    return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
}

int64_t steady_ms()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

bool track_open(const std::shared_ptr<rtc::Track> &t)
{
    try {
        return t && t->isOpen();
    } catch (const std::exception &) {
        return false;
    }
}

/* Say in the track's description which SSRC we send it from: with many tracks in one connection the Puddle can't otherwise
 * tell them apart (libdatachannel gives a packet it can't place to every track). */
uint32_t own_ssrc(const std::shared_ptr<rtc::Track> &track)
{
    uint32_t ssrc = random32() | 1;
    auto d = track->description();
    d.addSSRC(ssrc, std::string("pond"));
    track->setDescription(std::move(d));
    return ssrc;
}

/* What the Puddle sends us, which we only receive: Opus as whole frames, H.264 as whole access units with long start
 * codes. No SSRC of ours in their descriptions: we send nothing on them (and an answer would turn them down). */
void opus_in(const std::shared_ptr<rtc::Track> &track)
{
    track->setMediaHandler(std::make_shared<rtc::OpusRtpDepacketizer>());
}

void h264_in(const std::shared_ptr<rtc::Track> &track)
{
    track->setMediaHandler(std::make_shared<rtc::H264RtpDepacketizer>(rtc::NalUnit::Separator::LongStartSequence));
}

/* H.264 out (a camera to a watcher), cut into packets (FU-A) with sender reports */
void h264_out(const std::shared_ptr<rtc::Track> &track)
{
    auto rtp = std::make_shared<rtc::RtpPacketizationConfig>(own_ssrc(track), "pond", H264_PT, rtc::H264RtpPacketizer::ClockRate);
    auto pack = std::make_shared<rtc::H264RtpPacketizer>(rtc::NalUnit::Separator::StartSequence, rtp, 1200);
    pack->addToChain(std::make_shared<rtc::RtcpSrReporter>(rtp));
    track->setMediaHandler(pack);
}

/* An access unit a decoder can start from: it has an IDR slice, or the SPS that comes before one */
bool key_frame(const rtc::binary &au)
{
    for (size_t i = 0; i + 3 < au.size(); i++)
        if (au[i] == std::byte{0} && au[i + 1] == std::byte{0} && au[i + 2] == std::byte{1}) {
            int type = (int)au[i + 3] & 0x1f;
            if (type == 5 || type == 7)
                return true;
            i += 2;
        }
    return false;
}

int media_lines(const std::string &sdp)
{
    int n = 0;
    for (size_t at = 0; (at = sdp.find("m=", at)) != std::string::npos; at += 2)
        if ((at == 0 || sdp[at - 1] == '\n') && (!sdp.compare(at, 7, "m=audio") || !sdp.compare(at, 7, "m=video")))
            n++;
    return n;
}

/* The video track number in a mid "v1" .. "v16", or 0 */
int track_number(const std::string &mid)
{
    if (mid.size() < 2 || mid.size() > 3 || mid[0] != 'v' || mid[1] == '0')
        return 0;
    int n = 0;
    for (size_t i = 1; i < mid.size(); i++) {
        if (mid[i] < '0' || mid[i] > '9')
            return 0;
        n = n * 10 + (mid[i] - '0');
    }
    return n <= VOICE_TRACKS ? n : 0;
}

/* Run `fn` on a thread of its own and resume where the coroutine was (a loop thread) with what it gave: setting up a
 * connection waits on libdatachannel's threads, which a loop thread must not. */
struct OffLoop {
    std::function<Answer()> fn;
    Answer result;
    bool await_ready() { return false; }
    void await_suspend(std::coroutine_handle<> h)
    {
        trantor::EventLoop *loop = trantor::EventLoop::getEventLoopOfCurrentThread();
        std::thread([this, h, loop] {
            result = fn();
            if (loop)
                loop->queueInLoop([h] { h.resume(); });
            else
                h.resume();
        }).detach();
    }
    Answer await_resume() { return std::move(result); }
};

} // namespace

/* One member in one voice channel: their connection to us. */
struct Session {
    std::string id, member;
    int64_t channel = 0;
    int slot = 0;
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::Track> voice, camera_in, screen_in; /* theirs to us: held here, since the connection holds its tracks weakly */
    std::shared_ptr<rtc::DataChannel> dc;   /* the others' voices to them */
    std::shared_ptr<rtc::Track> out[VOICE_TRACKS + 1]; /* [n]: the camera or screen they watch on v<n> */
    uint32_t base[VOICE_TRACKS + 1] = {};   /* our timestamps on each are the sender's, moved by this much */
    bool waiting[VOICE_TRACKS + 1] = {};    /* hold back until a key frame: the start of a picture */
    std::string watching[VOICE_TRACKS + 1]; /* whose camera (a member's id), or screen ("<id>/screen") */
    bool dc_open = false, connected = false, announced = false, gone = false;
    bool muted = false, deafened = false, camera = false, screen = false;
    int64_t started = 0, lost_since = 0;
    Bucket audio_limit{20, 16};             /* about 60 frames a second */
    Bucket video_limit{30, 33};             /* about 30 a second */
    int64_t video_window = 0, video_bytes = 0; /* and 2 Mbit/s */
    Bucket screen_limit{30, 33};            /* a shared screen: as many frames */
    int64_t screen_window = 0, screen_bytes = 0; /* and 4 Mbit/s, for text to stay sharp */
};
using SessionPtr = std::shared_ptr<Session>;

struct Voice::Impl : std::enable_shared_from_this<Voice::Impl> {
    Service &svc;
    VoiceSettings set;
    trantor::EventLoop *loop;
    std::mutex mu;
    std::map<std::string, SessionPtr> sessions;  /* by id */
    std::map<std::string, SessionPtr> of_member; /* the one each member has */
    std::map<int64_t, std::vector<SessionPtr>> in_channel;
    /* events to send, in order: one coroutine sends them, one after another */
    std::deque<std::pair<std::string, std::string>> pending;
    bool sending = false, checking = false;
    /* connections to close: closing one can wait on its threads, so never in a callback of its own */
    std::deque<std::shared_ptr<rtc::PeerConnection>> closing;
    std::condition_variable wake;
    bool stopping = false;
    std::thread worker;

    Impl(Service &s, VoiceSettings v, trantor::EventLoop *l) : svc(s), set(std::move(v)), loop(l) {}

    std::string json_of(const Session &s) const
    {
        return "{\"channel\":" + std::to_string(s.channel) + ",\"member\":" + jstr(s.member) + ",\"slot\":" + std::to_string(s.slot) +
               ",\"muted\":" + (s.muted ? "true" : "false") + ",\"deafened\":" + (s.deafened ? "true" : "false") +
               ",\"camera\":" + (s.camera ? "true" : "false") + ",\"screen\":" + (s.screen ? "true" : "false") + "}";
    }

    /* lock held */
    void announce(std::string kind, std::string data)
    {
        pending.emplace_back(std::move(kind), std::move(data));
        if (sending || !loop)
            return;
        sending = true;
        std::weak_ptr<Impl> weak = shared_from_this();
        loop->queueInLoop([weak] {
            if (auto self = weak.lock())
                drogon::async_run([self]() -> Task<> { co_await self->send_events(); });
        });
    }

    Task<> send_events()
    {
        for (;;) {
            std::pair<std::string, std::string> e;
            {
                std::lock_guard<std::mutex> g(mu);
                if (pending.empty()) {
                    sending = false;
                    co_return;
                }
                e = std::move(pending.front());
                pending.pop_front();
            }
            co_await svc.voice_event(e.first, e.second);
        }
    }

    /* lock held */
    void index()
    {
        in_channel.clear();
        for (auto &[id, s] : sessions)
            in_channel[s->channel].push_back(s);
    }

    /* lock held. Let `s` go: out of the channel, its connection closed; the others told if they had been told it came. */
    void drop(const SessionPtr &s, const char *why)
    {
        if (s->gone)
            return;
        s->gone = true;
        sessions.erase(s->id);
        if (auto it = of_member.find(s->member); it != of_member.end() && it->second == s)
            of_member.erase(it);
        index();
        if (s->announced)
            announce("voice_left", "{\"channel\":" + std::to_string(s->channel) + ",\"member\":" + jstr(s->member) + "}");
        LOG_INFO << "voice: " << s->member.substr(0, 12) << "... out of channel " << s->channel << " (" << why << ")";
        close(s);
    }

    /* lock held */
    void close(const SessionPtr &s)
    {
        if (s->pc)
            closing.push_back(s->pc);
        s->pc.reset();
        wake.notify_all();
    }

    /* lock held. The smallest slot free in `channel`, not counting `except`; 0 if none. */
    int free_slot(int64_t channel, const SessionPtr &except)
    {
        std::set<int> held;
        if (auto it = in_channel.find(channel); it != in_channel.end())
            for (auto &s : it->second)
                if (s != except)
                    held.insert(s->slot);
        for (int n = 1; n <= VOICE_SLOTS; n++)
            if (!held.count(n))
                return n;
        return 0;
    }

    void on_voice(const std::weak_ptr<Session> &weak, const rtc::binary &frame, uint32_t ts)
    {
        if (frame.empty() || frame.size() > VOICE_FRAME_MAX)
            return;
        std::vector<std::shared_ptr<rtc::DataChannel>> to;
        rtc::binary msg;
        {
            std::lock_guard<std::mutex> g(mu);
            SessionPtr s = weak.lock();
            if (!s || s->gone || s->muted || !s->audio_limit.take(steady_ms()))
                return;
            auto it = in_channel.find(s->channel);
            if (it == in_channel.end())
                return;
            for (auto &r : it->second)
                if (r != s && r->dc_open && r->dc && !r->deafened)
                    to.push_back(r->dc);
            if (to.empty())
                return;
            msg.resize(6 + frame.size());
            msg[0] = (std::byte)(s->slot >> 8), msg[1] = (std::byte)(s->slot & 0xff);
            for (int i = 0; i < 4; i++)
                msg[2 + i] = (std::byte)(ts >> (24 - 8 * i));
            std::copy(frame.begin(), frame.end(), msg.begin() + 6);
        }
        for (auto &dc : to)
            try {
                dc->send(msg);
            } catch (const std::exception &) {
            }
    }

    /* A frame of a member's camera, or of the screen they share (`screen`): to each track watching it, from a key frame on */
    void on_video(const std::weak_ptr<Session> &weak, const rtc::binary &au, uint32_t ts, bool screen)
    {
        if (au.empty() || au.size() > VOICE_VIDEO_MAX)
            return;
        bool key = key_frame(au);
        std::vector<std::pair<std::shared_ptr<rtc::Track>, uint32_t>> to;
        {
            std::lock_guard<std::mutex> g(mu);
            SessionPtr s = weak.lock();
            if (!s || s->gone || !(screen ? s->screen : s->camera))
                return;
            int64_t now = steady_ms();
            int64_t &window = screen ? s->screen_window : s->video_window, &bytes = screen ? s->screen_bytes : s->video_bytes;
            if (now - window >= 1000)
                window = now, bytes = 0;
            if (!(screen ? s->screen_limit : s->video_limit).take(now) || (bytes += (int64_t)au.size()) > (screen ? 4000000 : 2000000) / 8)
                return;
            const std::string key_of = screen ? s->member + "/screen" : s->member;
            auto it = in_channel.find(s->channel);
            if (it == in_channel.end())
                return;
            for (auto &r : it->second) {
                if (r == s)
                    continue;
                for (int n = 1; n <= VOICE_TRACKS; n++) {
                    if (r->watching[n] != key_of || !track_open(r->out[n]))
                        continue;
                    if (r->waiting[n] && !key)
                        continue;
                    r->waiting[n] = false;
                    to.emplace_back(r->out[n], r->base[n] + ts);
                }
            }
        }
        for (auto &[track, t] : to)
            try {
                track->sendFrame(au, rtc::FrameInfo(t));
            } catch (const std::exception &) {
            }
    }

    void on_state(const std::weak_ptr<Session> &weak, rtc::PeerConnection::State st)
    {
        std::lock_guard<std::mutex> g(mu);
        SessionPtr s = weak.lock();
        if (!s || s->gone)
            return;
        if (st == rtc::PeerConnection::State::Connected) {
            s->connected = true;
            s->lost_since = 0;
            if (!s->announced) {
                s->announced = true;
                announce("voice", json_of(*s));
                LOG_INFO << "voice: " << s->member.substr(0, 12) << "... in channel " << s->channel << " (slot " << s->slot << ")";
            }
        } else if (st == rtc::PeerConnection::State::Disconnected || st == rtc::PeerConnection::State::Failed ||
                   st == rtc::PeerConnection::State::Closed) {
            /* may come back, or the Puddle may join again with a new connection (restarted, or on another network): until
             * lost_ms has passed, that takes this one's place without anyone being told it left. Leaving is a route. */
            if (!s->lost_since)
                s->lost_since = steady_ms();
        }
    }

    /* The connection for a join: answers the Puddle's offer. On its own thread (OffLoop). */
    Answer connect(std::string member, int64_t channel, std::string offer, int udp, std::string peer_ip)
    {
        auto s = std::make_shared<Session>();
        s->id = new_token();
        s->member = member;
        s->channel = channel;
        s->started = steady_ms();
        for (auto &b : s->base)
            b = random32();
        {
            std::lock_guard<std::mutex> g(mu);
            if (stopping)
                return fail(503, "busy");
            SessionPtr old = of_member.count(member) ? of_member[member] : nullptr;
            if (old && old->channel == channel) { /* again, in the same channel: a new connection, the same place */
                s->slot = old->slot;
                s->muted = old->muted, s->deafened = old->deafened, s->camera = old->camera, s->screen = old->screen;
            } else {
                size_t in = in_channel.count(channel) ? in_channel[channel].size() : 0;
                if (set.max_in_channel && in >= set.max_in_channel)
                    return fail(409, "full");
                s->slot = free_slot(channel, nullptr);
                if (!s->slot)
                    return fail(409, "full");
            }
        }

        rtc::Configuration conf; /* no ICE servers: the Puddle reaches us at our own address (docs/voice.md) */
        conf.enableIceUdpMux = true;
        conf.disableAutoNegotiation = true; /* the answer waits until each track says its SSRC */
        conf.portRangeBegin = conf.portRangeEnd = (uint16_t)set.port;
        if (!set.bind.empty())
            conf.bindAddress = set.bind;
        std::shared_ptr<rtc::PeerConnection> pc;
        std::string sdp;
        try {
            pc = std::make_shared<rtc::PeerConnection>(conf);
            s->pc = pc;
            std::weak_ptr<Session> weak = s;
            std::weak_ptr<Impl> self = shared_from_this();
            auto seen = std::make_shared<std::atomic<int>>(0);
            pc->onStateChange([self, weak](rtc::PeerConnection::State st) {
                if (auto me = self.lock())
                    me->on_state(weak, st);
            });
            pc->onTrack([self, weak, seen](std::shared_ptr<rtc::Track> track) {
                struct Count {
                    std::shared_ptr<std::atomic<int>> n;
                    ~Count() { (*n)++; }
                } counted{seen};
                auto me = self.lock();
                if (!me)
                    return;
                const std::string mid = track->mid();
                if (mid == "audio" || mid == "video" || mid == "screen") {
                    std::lock_guard<std::mutex> g(me->mu);
                    if (SessionPtr s = weak.lock())
                        (mid == "audio" ? s->voice : mid == "video" ? s->camera_in : s->screen_in) = track;
                }
                if (mid == "audio") {
                    opus_in(track);
                    track->onFrame([self, weak](rtc::binary data, rtc::FrameInfo info) {
                        if (auto me = self.lock())
                            me->on_voice(weak, data, info.timestamp);
                    });
                } else if (mid == "video" || mid == "screen") {
                    h264_in(track);
                    const bool screen = mid == "screen";
                    track->onFrame([self, weak, screen](rtc::binary data, rtc::FrameInfo info) {
                        if (auto me = self.lock())
                            me->on_video(weak, data, info.timestamp, screen);
                    });
                } else if (int n = track_number(mid)) {
                    h264_out(track);
                    std::lock_guard<std::mutex> g(me->mu);
                    if (SessionPtr s = weak.lock())
                        s->out[n] = track;
                }
                /* anything else stays as it came, and gets nothing */
            });
            pc->onDataChannel([self, weak](std::shared_ptr<rtc::DataChannel> dc) {
                auto me = self.lock();
                if (!me || dc->label() != "frames")
                    return;
                {
                    std::lock_guard<std::mutex> g(me->mu);
                    if (SessionPtr s = weak.lock())
                        s->dc = dc;
                }
                dc->onOpen([self, weak] {
                    auto me = self.lock();
                    if (!me)
                        return;
                    std::lock_guard<std::mutex> g(me->mu);
                    if (SessionPtr s = weak.lock())
                        s->dc_open = true;
                });
                dc->onClosed([self, weak] {
                    auto me = self.lock();
                    if (!me)
                        return;
                    std::lock_guard<std::mutex> g(me->mu);
                    if (SessionPtr s = weak.lock())
                        s->dc_open = false;
                });
            });
            pc->setRemoteDescription(rtc::Description(offer, "offer"));
            /* where the request came from, at the port the Puddle's calls use: behind NAT it won't answer, and the address
             * its first check comes from will (a peer-reflexive candidate) */
            if (udp > 0 && !peer_ip.empty()) {
                try {
                    pc->addRemoteCandidate(rtc::Candidate("a=candidate:9 1 UDP 2122000000 " + peer_ip + " " + std::to_string(udp) + " typ host",
                                                          "audio"));
                } catch (const std::exception &) {
                }
            }
            const int want = media_lines(offer);
            for (int i = 0; i < 500 && seen->load() < want; i++)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            if (seen->load() >= want) {
                pc->setLocalDescription();
                for (int i = 0; i < 500; i++) {
                    if (pc->gatheringState() == rtc::PeerConnection::GatheringState::Complete && pc->localDescription()) {
                        sdp = std::string(*pc->localDescription());
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            }
        } catch (const std::exception &e) {
            LOG_INFO << "voice: an offer from " << member.substr(0, 12) << "... can't be used: " << e.what();
        }
        std::lock_guard<std::mutex> g(mu);
        if (sdp.empty() || stopping || s->gone) {
            if (pc)
                pc->resetCallbacks();
            s->gone = true;
            close(s);
            return sdp.empty() ? fail(400, "bad_sdp") : stopping ? fail(503, "busy") : fail(502, "failed");
        }
        /* in: this member's earlier connection goes; one in another channel leaves it */
        if (auto it = of_member.find(member); it != of_member.end()) {
            SessionPtr old = it->second;
            if (old->channel == channel && old->announced) {
                s->announced = true; /* the same place: nobody needs telling again */
                old->announced = false;
            }
            drop(old, "joined again");
        }
        /* someone else came in meanwhile and took the slot: the next free one (nobody has been told this one yet) */
        if (auto it = in_channel.find(channel); it != in_channel.end())
            for (auto &o : it->second)
                if (o->slot == s->slot) {
                    s->slot = free_slot(channel, nullptr);
                    break;
                }
        sessions[s->id] = s;
        of_member[member] = s;
        index();
        if (s->connected && !s->announced) { /* it came up before we got here */
            s->announced = true;
            announce("voice", json_of(*s));
        }
        return Answer{200, "{\"session\":" + jstr(s->id) + ",\"sdp\":" + jstr(sdp) + ",\"udp\":" + std::to_string(set.port) +
                               ",\"slot\":" + std::to_string(s->slot) + "}"};
    }

    /* lock held: the session `id`, if it is `member`'s */
    SessionPtr mine(const std::string &member, const std::string &id)
    {
        auto it = sessions.find(id);
        return it != sessions.end() && it->second->member == member ? it->second : nullptr;
    }

    void work()
    {
        std::unique_lock<std::mutex> lk(mu);
        int64_t next_check = 0;
        for (;;) {
            while (!closing.empty()) {
                auto pc = std::move(closing.front());
                closing.pop_front();
                lk.unlock();
                try {
                    pc->resetCallbacks();
                    pc->close();
                } catch (const std::exception &) {
                }
                pc.reset();
                lk.lock();
            }
            if (stopping)
                break;
            int64_t now = steady_ms();
            std::vector<SessionPtr> late;
            for (auto &[id, s] : sessions)
                if ((!s->connected && now - s->started >= set.connect_ms) || (s->lost_since && now - s->lost_since >= set.lost_ms))
                    late.push_back(s);
            for (auto &s : late)
                drop(s, s->connected ? "connection lost" : "never connected");
            if (now >= next_check && !checking && loop && !sessions.empty()) {
                next_check = now + 1000;
                checking = true;
                std::weak_ptr<Impl> weak = shared_from_this();
                loop->queueInLoop([weak] {
                    if (auto self = weak.lock())
                        drogon::async_run([self]() -> Task<> { co_await self->check(); });
                });
            }
            wake.wait_for(lk, std::chrono::milliseconds(250));
        }
    }

    /* Members removed, banned or gone, and channels deleted: done by pondctl or another route, so asked of the database */
    Task<> check()
    {
        std::vector<std::string> members;
        std::vector<int64_t> channels;
        {
            std::lock_guard<std::mutex> g(mu);
            for (auto &[m, s] : of_member)
                members.push_back(m);
            for (auto &[c, list] : in_channel)
                channels.push_back(c);
        }
        try {
            auto [gone_members, gone_channels] = co_await svc.voice_gone(members, channels);
            std::lock_guard<std::mutex> g(mu);
            std::vector<SessionPtr> out;
            for (auto &[id, s] : sessions)
                if (std::find(gone_members.begin(), gone_members.end(), s->member) != gone_members.end() ||
                    std::find(gone_channels.begin(), gone_channels.end(), s->channel) != gone_channels.end())
                    out.push_back(s);
            for (auto &s : out)
                drop(s, "no longer a member, or the channel went");
        } catch (const std::exception &e) {
            LOG_ERROR << "voice: " << e.what();
        }
        std::lock_guard<std::mutex> g(mu);
        checking = false;
    }
};

Voice::Voice(Service &svc, VoiceSettings settings, trantor::EventLoop *loop) : impl_(std::make_shared<Impl>(svc, std::move(settings), loop)) {}

Voice::~Voice()
{
    stop();
}

bool Voice::built()
{
    return true;
}

void Voice::start()
{
    std::lock_guard<std::mutex> g(impl_->mu);
    if (!impl_->worker.joinable() && !impl_->stopping)
        impl_->worker = std::thread([impl = impl_] { impl->work(); });
}

void Voice::stop()
{
    std::thread worker;
    {
        std::lock_guard<std::mutex> g(impl_->mu);
        if (impl_->stopping)
            return;
        for (auto &[id, s] : impl_->sessions) {
            s->gone = true;
            impl_->close(s);
        }
        impl_->sessions.clear();
        impl_->of_member.clear();
        impl_->in_channel.clear();
        impl_->stopping = true;
        impl_->wake.notify_all();
        worker = std::move(impl_->worker);
    }
    if (worker.joinable())
        worker.join();
    /* what the worker hadn't closed yet */
    std::deque<std::shared_ptr<rtc::PeerConnection>> left;
    {
        std::lock_guard<std::mutex> g(impl_->mu);
        left.swap(impl_->closing);
    }
    for (auto &pc : left)
        try {
            pc->resetCallbacks();
            pc->close();
        } catch (const std::exception &) {
        }
}

Task<Answer> Voice::join(std::string member, int64_t channel, std::string sdp, int udp, std::string peer_ip)
{
    if (!co_await impl_->svc.is_voice_channel(channel))
        co_return fail(404, "no_channel");
    auto impl = impl_;
    co_return co_await OffLoop{[impl, member, channel, sdp, udp, peer_ip] { return impl->connect(member, channel, sdp, udp, peer_ip); }, {}};
}

Answer Voice::state(const std::string &member, const std::string &session, std::optional<bool> muted, std::optional<bool> deafened,
                    std::optional<bool> camera, std::optional<bool> screen)
{
    std::lock_guard<std::mutex> g(impl_->mu);
    SessionPtr s = impl_->mine(member, session);
    if (!s)
        return fail(404, "no_session");
    bool changed = (muted && *muted != s->muted) || (deafened && *deafened != s->deafened) || (camera && *camera != s->camera) ||
                   (screen && *screen != s->screen);
    s->screen = screen.value_or(s->screen);
    s->muted = muted.value_or(s->muted);
    s->deafened = deafened.value_or(s->deafened);
    s->camera = camera.value_or(s->camera);
    if (changed && s->announced)
        impl_->announce("voice", impl_->json_of(*s));
    return Answer{204, ""};
}

Answer Voice::watch(const std::string &member, const std::string &session, const std::map<int, std::string> &tracks)
{
    std::lock_guard<std::mutex> g(impl_->mu);
    SessionPtr s = impl_->mine(member, session);
    if (!s)
        return fail(404, "no_session");
    for (auto &[n, who] : tracks) {
        if (n < 1 || n > VOICE_TRACKS || s->watching[n] == who)
            continue;
        s->watching[n] = who;
        s->waiting[n] = true; /* a new picture on this track starts at a key frame */
    }
    return Answer{204, ""};
}

Answer Voice::leave(const std::string &member, const std::string &session)
{
    std::lock_guard<std::mutex> g(impl_->mu);
    SessionPtr s = impl_->mine(member, session);
    if (!s)
        return fail(404, "no_session");
    impl_->drop(s, "left");
    return Answer{204, ""};
}

std::string Voice::now_json()
{
    std::lock_guard<std::mutex> g(impl_->mu);
    std::string out = "[";
    for (auto &[c, list] : impl_->in_channel)
        for (auto &s : list)
            if (s->announced)
                out += (out.size() > 1 ? "," : "") + impl_->json_of(*s);
    return out + "]";
}

#endif

} // namespace pond
