#pragma once

/* pond.yaml: where pondd listens, what its invite links say, where its files are and which database it uses
 * (docs/design.md, "pond.yaml"). pondd and pondctl read the same file. */

#include <string>
#include <vector>

namespace pond {

struct DbConfig {
    std::string host = "127.0.0.1"; /* "localhost" means the client library's default Unix socket */
    unsigned port = 3306;
    std::string user = "pond";
    std::string password;
    std::string name = "pond";
    unsigned connections = 16;
};

/* Voice channels (docs/voice.md): `voice: false` turns them off; otherwise a map, every key optional. */
struct VoiceConfig {
    bool on = true;
    unsigned max_in_channel = 0; /* people in one voice channel; 0 = no limit (but the slots: 65535) */
};

struct Config {
    std::string listen = "0.0.0.0";
    /* ports: chat and voice, the same number on two protocols unless said (docs/design.md, "pond.yaml") */
    unsigned chat_port = 443;  /* TCP: HTTPS, everything but voice (joining, channels, messages, files) */
    unsigned voice_port = 0;   /* UDP: voice and video in voice channels; load_config sets it to chat_port when not given */
    std::string address; /* host[:port] for invite links; "" = listen:port */
    std::string data_dir;
    unsigned threads = 0;       /* threads answering requests; 0 = one per CPU */
    int events_wait_ms = 25000; /* how long a request for events is held open */
    DbConfig db;
    VoiceConfig voice;
    std::vector<std::string> notes; /* settings under an older name, still read: pondd logs these so they get renamed */
};

/* Read `path`. false with the reason in `err`. Unknown keys are an error, so a typo is not silently ignored. */
bool load_config(const std::string &path, Config &out, std::string &err);

/* The address invite links carry: `address`, or listen:port when that is a real address. */
std::string link_address(const Config &c);

} // namespace pond
