/* Voice channels (docs/voice.md) against a real pondd: members connect with libdatachannel the way a Puddle does (an offer with
 * an Opus track, a camera track, v1 .. v16 to watch on and a `frames` data channel), and the Pond forwards. Who is in a channel,
 * every voice to everyone else with the sender's slot, muted and deafened, cameras only to those watching and only from a key
 * frame, leaving, being removed, and a channel's kind. */

#include "harness.h"
#include "voice.h"

#ifndef POND_HAVE_VOICE
int main()
{
    printf("voice_test: skipped (built without voice)\n");
    return 77;
}
#else

#include <rtc/rtc.hpp>

#include <atomic>
#include <map>
#include <mutex>

using namespace pond;

static std::string join_body(const Identity &who, const std::string &invite, const char *name)
{
    return "{\"invite\":" + jstr(invite) + ",\"chain\":" + jstr(who.chain) + ",\"profile\":{\"display_name\":\"" + name + "\"}}";
}

static std::string invite(PondProc &p)
{
    std::string out;
    p.ctl("invite create 2>/dev/null", &out);
    size_t at = out.find("invite=");
    return at == std::string::npos ? "" : out.substr(at + 7, 26);
}

static rtc::binary bytes(const std::string &s)
{
    rtc::binary b(s.size());
    memcpy(b.data(), s.data(), s.size());
    return b;
}

static std::string text_of(const rtc::binary &b)
{
    return std::string((const char *)b.data(), b.size());
}

/* An access unit: a key frame (SPS, PPS, IDR) or not (one slice), `size` bytes long, its frame number in byte 20 */
static std::string access_unit(bool key, int number, size_t size = 6000)
{
    std::string au;
    if (key)
        au += std::string("\x00\x00\x00\x01\x67\x42\xc0\x1e\x00\x00\x00\x01\x68\xce\x3c\x80\x00\x00\x00\x01\x65", 21);
    else
        au += std::string("\x00\x00\x00\x01\x41\x9a\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00", 21);
    au[20] = (char)number;
    while (au.size() < size)
        au += (char)(0x10 + au.size() % 200); /* never two zeros in a row: no start code by accident */
    return au;
}

/* A member's side of voice: the connection a Puddle makes to the Pond. */
struct Member {
    Identity id;
    std::string name;
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::Track> audio, video, screen, watch[VOICE_TRACKS + 1];
    std::shared_ptr<rtc::DataChannel> dc;
    std::atomic<bool> connected{false}, dc_open{false};
    std::mutex mu;
    std::vector<std::pair<int, std::string>> heard;    /* slot, frame */
    std::map<int, std::vector<std::string>> pictures;  /* track, access units */
    std::string session;
    int slot = 0;
    uint32_t ts = 1000;

    explicit Member(const char *n) : name(n) {}

    static uint32_t own_ssrc(const std::shared_ptr<rtc::Track> &t, uint32_t ssrc)
    {
        auto d = t->description();
        d.addSSRC(ssrc, std::string("member"));
        t->setDescription(std::move(d));
        return ssrc;
    }

    /* Make the offer, join `channel`, and answer whatever the Pond said */
    Reply join(PondProc &pond, int64_t channel)
    {
        close();
        connected = dc_open = false;
        rtc::Configuration conf;
        conf.enableIceUdpMux = true;
        conf.disableAutoNegotiation = true;
        unsigned udp = free_port();
        conf.portRangeBegin = conf.portRangeEnd = (uint16_t)udp;
        conf.bindAddress = "127.0.0.1";
        pc = std::make_shared<rtc::PeerConnection>(conf);
        pc->onStateChange([this](rtc::PeerConnection::State s) { connected = s == rtc::PeerConnection::State::Connected; });

        rtc::Description::Audio a("audio", rtc::Description::Direction::SendOnly);
        a.addOpusCodec(111);
        audio = pc->addTrack(a);
        auto artp = std::make_shared<rtc::RtpPacketizationConfig>(own_ssrc(audio, 1000 + (uint32_t)name.size() * 2 + 1), "member", 111,
                                                                  rtc::OpusRtpPacketizer::DefaultClockRate);
        audio->setMediaHandler(std::make_shared<rtc::OpusRtpPacketizer>(artp));

        for (int n = -1; n <= VOICE_TRACKS; n++) { /* -1: the screen shared, 0: the camera, 1 .. 16: what is watched */
            std::string mid = n > 0 ? "v" + std::to_string(n) : n == 0 ? "video" : "screen";
            rtc::Description::Video v(mid, n > 0 ? rtc::Description::Direction::RecvOnly : rtc::Description::Direction::SendOnly);
            v.addH264Codec(96);
            auto t = pc->addTrack(v);
            auto rtp = std::make_shared<rtc::RtpPacketizationConfig>(own_ssrc(t, 5003 + (uint32_t)n * 2), "member", 96,
                                                                     rtc::H264RtpPacketizer::ClockRate);
            auto pack = std::make_shared<rtc::H264RtpPacketizer>(rtc::NalUnit::Separator::StartSequence, rtp, 1200);
            pack->addToChain(std::make_shared<rtc::RtcpSrReporter>(rtp));
            pack->addToChain(std::make_shared<rtc::H264RtpDepacketizer>(rtc::NalUnit::Separator::LongStartSequence));
            t->setMediaHandler(pack);
            if (n < 0) {
                screen = t;
            } else if (n) {
                watch[n] = t;
                t->onFrame([this, n](rtc::binary data, rtc::FrameInfo) {
                    std::lock_guard<std::mutex> g(mu);
                    pictures[n].push_back(text_of(data));
                });
            } else {
                video = t;
            }
        }
        rtc::DataChannelInit init;
        init.reliability.unordered = true;
        init.reliability.maxRetransmits = 0;
        dc = pc->createDataChannel("frames", init);
        dc->onOpen([this] { dc_open = true; });
        dc->onMessage([this](rtc::message_variant m) {
            if (auto *b = std::get_if<rtc::binary>(&m); b && b->size() >= 6) {
                int s = (int)(*b)[0] << 8 | (int)(*b)[1];
                std::lock_guard<std::mutex> g(mu);
                heard.emplace_back(s, std::string((const char *)b->data() + 6, b->size() - 6));
            }
        });
        pc->setLocalDescription();
        until([&] { return pc->gatheringState() == rtc::PeerConnection::GatheringState::Complete; }, 5000);
        std::string offer = std::string(*pc->localDescription());
        std::string body = "{\"sdp\":" + jstr(offer) + ",\"udp\":" + std::to_string(udp) + "}";
        Reply r = pond.call(&id, "POST", "/pond/v1/channels/" + std::to_string(channel) + "/voice/join", body);
        if (r.status == 200) {
            Json::Value v = parse(r.body);
            session = v["session"].asString();
            slot = v["slot"].asInt();
            if (getenv("VOICE_TEST_SDP"))
                fprintf(stderr, "OFFER\n%s\nANSWER\n%s\n", offer.c_str(), v["sdp"].asString().c_str());
            pc->setRemoteDescription(rtc::Description(v["sdp"].asString(), "answer"));
            /* the Pond's address, at the port it said: what a Puddle adds itself */
            pc->addRemoteCandidate(rtc::Candidate("a=candidate:9 1 UDP 2122000000 127.0.0.1 " + std::to_string(v["udp"].asInt()) + " typ host",
                                                  "audio"));
        }
        return r;
    }

    bool up()
    {
        bool ok = until([&] { return connected && dc_open && audio->isOpen() && video->isOpen() && screen->isOpen(); }, 15000);
        if (!ok)
            fprintf(stderr, "%s: connected %d, frames %d, audio %d, video %d\n", name.c_str(), (int)connected, (int)dc_open,
                    (int)audio->isOpen(), (int)video->isOpen());
        return ok;
    }

    void say(const std::string &frame)
    {
        try {
            audio->sendFrame(bytes(frame), rtc::FrameInfo(ts));
        } catch (const std::exception &) {
        }
        ts += 960;
    }

    void show(const std::string &au, uint32_t at, bool on_screen = false)
    {
        try {
            (on_screen ? screen : video)->sendFrame(bytes(au), rtc::FrameInfo(at));
        } catch (const std::exception &) {
        }
    }

    /* the frames heard from `slot` */
    std::vector<std::string> from(int s)
    {
        std::lock_guard<std::mutex> g(mu);
        std::vector<std::string> out;
        for (auto &[k, f] : heard)
            if (k == s)
                out.push_back(f);
        return out;
    }

    size_t heard_count()
    {
        std::lock_guard<std::mutex> g(mu);
        return heard.size();
    }

    std::vector<std::string> on(int track)
    {
        std::lock_guard<std::mutex> g(mu);
        return pictures[track];
    }

    void forget()
    {
        std::lock_guard<std::mutex> g(mu);
        heard.clear();
        pictures.clear();
    }

    void close()
    {
        if (pc) {
            pc->resetCallbacks();
            pc->close();
            pc.reset();
        }
    }

    ~Member() { close(); }
};

/* Every event after `cursor` (moved past them), as their JSON, until there are none for a moment */
static std::string events(PondProc &pond, const Identity &who, int64_t &cursor)
{
    std::string all;
    for (;;) {
        Reply a = pond.call(&who, "GET", "/pond/v1/events?after=" + std::to_string(cursor));
        if (a.status != 200 || has(a.body, "\"events\":[]"))
            return all;
        all += a.body;
        cursor = num_of(a.body, "cursor");
    }
}

/* Wait for events after `cursor` until `want` is among them */
static bool event_with(PondProc &pond, const Identity &who, int64_t &cursor, const std::string &want, std::string *seen = nullptr)
{
    std::string all;
    bool ok = until([&] {
        all += events(pond, who, cursor);
        return has(all, want);
    }, 15000);
    if (seen)
        *seen = all;
    return ok;
}

int main()
{
    if (!Voice::built()) {
        printf("voice_test: skipped (built without voice)\n");
        return 77;
    }
    TestDb db;
    if (!db.start()) {
        fprintf(stderr, "cannot start a MariaDB for the test\n");
        return 1;
    }
    PondProc pond(db);
    CHECK(pond.init("Voice Pond", 300));
    std::string out;
    CHECK(pond.ctl("channel create talk --voice", &out) == 0);
    CHECK(pond.ctl("channel list", &out) == 0 && has(out, "#talk") && has(out, "voice") && has(out, "#general"));
    CHECK(pond.up());

    Member ann("Ann"), ben("Ben"), cat("Cat");
    Identity stranger;
    for (Member *m : {&ann, &ben, &cat})
        CHECK(pond.call(&m->id, "POST", "/pond/v1/join", join_body(m->id, invite(pond), m->name.c_str()), "*").status == 200);

    /* ---- a channel's kind; protocol 4, with voice ---- */
    Reply a = pond.call(&ann.id, "GET", "/pond/v1/info");
    Json::Value info = parse(a.body);
    CHECK(info["protocol"].asInt() == 12 && has(a.body, "\"receipts\",\"sealed\",\"pictures\",\"channel_roles\",\"role_permissions\",\"pages\",\"role_colors\",\"voice\",\"dms\",\"dm_files\"]") && has(a.body, "\"voice\":[]"));
    int64_t talk = 0, general = 0;
    for (const auto &c : info["channels"])
        (c["name"].asString() == "talk" ? talk : general) = c["id"].asInt64();
    CHECK(talk > 0 && general > 0);
    CHECK(has(a.body, "\"name\":\"general\",\"topic\":\"\",\"position\":0,\"kind\":\"text\""));
    CHECK(has(a.body, "\"name\":\"talk\",\"topic\":\"\",\"position\":1,\"kind\":\"voice\""));
    int64_t cursor = info["cursor"].asInt64();

    /* ---- joining: only a voice channel, only a member ---- */
    CHECK(ann.join(pond, general).status == 404);
    CHECK(ann.join(pond, 9999).status == 404);
    {
        Reply r = pond.call(&stranger, "POST", "/pond/v1/channels/" + std::to_string(talk) + "/voice/join", "{\"sdp\":\"x\",\"udp\":1}");
        CHECK(r.status == 404 && r.body == "{\"error\":\"not_found\"}"); /* a stranger: like a path that isn't there */
    }
    CHECK(pond.call(&ann.id, "POST", "/pond/v1/channels/" + std::to_string(talk) + "/voice/join", "{\"sdp\":\"x\",\"udp\":1}").status == 400);

    Reply j = ann.join(pond, talk);
    CHECK(j.status == 200 && ann.slot == 1 && ann.session.size() == 26 && !j.receipt.empty());
    CHECK(ben.join(pond, talk).status == 200 && ben.slot == 2);
    CHECK(cat.join(pond, talk).status == 200 && cat.slot == 3);
    CHECK(ann.up() && ben.up() && cat.up());
    {
        std::string seen;
        CHECK(event_with(pond, ann.id, cursor, "\"slot\":3", &seen));
        CHECK(count(seen, "\"kind\":\"voice\"") == 3 && has(seen, "\"member\":\"" + ben.id.id + "\",\"slot\":2,\"muted\":false"));
        a = pond.call(&cat.id, "GET", "/pond/v1/info");
        CHECK(count(a.body, "\"channel\":" + std::to_string(talk)) == 3);
    }

    /* ---- every voice to everyone else, with the sender's slot ---- */
    for (int k = 0; k < 10; k++)
        ann.say("ann-" + std::to_string(k));
    CHECK(until([&] { return ben.from(1).size() >= 9 && cat.from(1).size() >= 9; }));
    CHECK(ben.from(1).size() > 0 && ben.from(1)[0].rfind("ann-", 0) == 0);
    CHECK(ann.heard_count() == 0); /* nobody hears themselves */
    ben.say("ben-0");
    CHECK(until([&] { return !ann.from(2).empty() && !cat.from(2).empty(); }));

    /* ---- muted: nothing of theirs goes on; deafened: nothing comes to them ---- */
    auto state = [&](Member &m, const std::string &body) {
        return pond.call(&m.id, "POST", "/pond/v1/voice/" + m.session + "/state", body).status;
    };
    CHECK(state(ben, "{\"muted\":true}") == 204);
    CHECK(state(cat, "{\"deafened\":true}") == 204);
    CHECK(state(ann, "{\"muted\":\"yes\"}") == 400);
    CHECK(pond.call(&ann.id, "POST", "/pond/v1/voice/" + ben.session + "/state", "{\"muted\":false}").status == 404); /* not hers */
    CHECK(event_with(pond, ann.id, cursor, "\"deafened\":true"));
    ann.forget(), ben.forget(), cat.forget();
    for (int k = 0; k < 5; k++)
        ben.say("ben-muted"), ann.say("ann-again");
    CHECK(until([&] { return ben.from(1).size() >= 4; }));
    until([] { return false; }, 300);
    CHECK(ann.from(2).empty() && cat.from(2).empty()); /* Ben is muted */
    CHECK(cat.heard_count() == 0);                     /* Cat is deafened */
    CHECK(state(ben, "{\"muted\":false}") == 204 && state(cat, "{\"deafened\":false}") == 204);

    /* ---- cameras: only to those watching, from a key frame on ---- */
    auto watch = [&](Member &m, const std::string &tracks) {
        return pond.call(&m.id, "POST", "/pond/v1/voice/" + m.session + "/watch", "{\"tracks\":" + tracks + "}").status;
    };
    CHECK(watch(ben, "{\"v1\":\"" + ann.id.id + "\"}") == 204);
    CHECK(watch(ben, "{\"v17\":\"" + ann.id.id + "\"}") == 400 && watch(ben, "{\"v1\":\"nobody\"}") == 400);
    ann.show(access_unit(true, 1), 90000); /* the camera is off: goes nowhere */
    until([] { return false; }, 300);
    CHECK(ben.on(1).empty());
    CHECK(state(ann, "{\"camera\":true}") == 204);
    CHECK(event_with(pond, ben.id, cursor, "\"camera\":true"));
    ann.show(access_unit(false, 2), 96000); /* no key frame yet: held back */
    until([] { return false; }, 200);
    ann.show(access_unit(true, 3), 102000);
    until([] { return false; }, 100);
    ann.show(access_unit(false, 4), 108000);
    CHECK(until([&] { return ben.on(1).size() >= 2; }));
    {
        auto got = ben.on(1);
        CHECK(got.size() == 2 && got[0].size() == 6000 && got[0][20] == 3 && got[1][20] == 4); /* whole, from the key frame */
        CHECK(got[0] == access_unit(true, 3));
    }
    CHECK(cat.on(1).empty() && ann.on(1).empty()); /* nobody else watches */
    CHECK(watch(ben, "{\"v1\":null}") == 204);
    ann.show(access_unit(true, 5), 114000);
    until([] { return false; }, 300);
    CHECK(ben.on(1).size() == 2);

    /* ---- a shared screen: its own track, watched as "<id>/screen", apart from the camera ---- */
    CHECK(watch(ben, "{\"v2\":\"" + ann.id.id + "/screen\"}") == 204);
    CHECK(watch(ben, "{\"v3\":\"nobody/screen\"}") == 400);
    ann.show(access_unit(true, 20), 200000, true); /* not sharing yet: goes nowhere */
    until([] { return false; }, 300);
    CHECK(ben.on(2).empty());
    CHECK(state(ann, "{\"screen\":true}") == 204);
    CHECK(event_with(pond, ben.id, cursor, "\"screen\":true"));
    ann.show(access_unit(false, 21), 206000, true); /* waits for a key frame */
    until([] { return false; }, 200);
    ann.show(access_unit(true, 22, 30000), 212000, true); /* a screen's key frame is big */
    ann.show(access_unit(true, 23), 218000);             /* the camera: not on v2 */
    CHECK(until([&] { return ben.on(2).size() >= 1; }));
    until([] { return false; }, 300);
    {
        auto got = ben.on(2);
        CHECK(got.size() == 1 && got[0].size() == 30000 && got[0][20] == 22);
    }
    CHECK(state(ann, "{\"screen\":false}") == 204);

    /* ---- a voice channel takes text too ---- */
    CHECK(pond.call(&ann.id, "POST", "/pond/v1/channels/" + std::to_string(talk) + "/send", "{\"text\":\"a link\"}").status == 200);

    /* ---- leaving; joining again keeps the slot, unannounced ---- */
    CHECK(pond.call(&cat.id, "POST", "/pond/v1/voice/" + cat.session + "/leave").status == 204);
    CHECK(pond.call(&cat.id, "POST", "/pond/v1/voice/" + cat.session + "/leave").status == 404);
    CHECK(event_with(pond, ann.id, cursor, "voice_left"));
    a = pond.call(&ann.id, "GET", "/pond/v1/info");
    CHECK(count(a.body, "\"channel\":" + std::to_string(talk)) == 2);
    std::string old = ann.session;
    CHECK(ann.join(pond, talk).status == 200 && ann.slot == 1 && ann.session != old && ann.up());
    CHECK(pond.call(&ann.id, "POST", "/pond/v1/voice/" + old + "/state", "{\"muted\":true}").status == 404); /* the old one is gone */
    {
        std::string seen = events(pond, ann.id, cursor);
        CHECK(!has(seen, "voice_left")); /* she never left */
    }
    ann.forget(), ben.forget();
    ann.say("ann-back");
    CHECK(until([&] { return !ben.from(1).empty(); }));

    /* ---- someone removed is out of voice too (pondctl, another process: the Pond notices) ---- */
    CHECK(pond.ctl("member remove " + ben.id.id, &out) == 0);
    CHECK(event_with(pond, ann.id, cursor, "\"kind\":\"voice_left\",\"data\":{\"channel\":" + std::to_string(talk) + ",\"member\":\"" + ben.id.id));

    /* ---- a channel deleted empties it ---- */
    CHECK(pond.ctl("channel delete talk", &out) == 0);
    CHECK(event_with(pond, ann.id, cursor, "\"member\":\"" + ann.id.id + "\"}"));
    a = pond.call(&ann.id, "GET", "/pond/v1/info");
    CHECK(has(a.body, "\"voice\":[]"));

    /* ---- pondd stops at once, connections and all ---- */
    {
        auto t0 = std::chrono::steady_clock::now();
        pond.down();
        CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(3));
    }
    return finish("voice_test");
}

#endif
