#pragma once

/* What a Pond does, apart from HTTP: members joining and leaving, channels, messages, invites and the event log that members'
 * Puddles follow (docs/design.md). pondd's routes call it for members; pondctl calls it for the admin. Every answer is JSON
 * text, in the shapes api/pond.yaml gives. */

#include "db.h"
#include "key.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

typedef struct puddle_keyed puddle_keyed;

namespace pond {

constexpr size_t TEXT_MAX = 4000;        /* characters in a message */
constexpr size_t TOPIC_MAX = 1024;
constexpr size_t POND_NAME_MAX = 100;    /* the Pond's name */
constexpr size_t DESCRIPTION_MAX = 1000;
constexpr size_t PROFILE_MAX = 16 * 1024;
constexpr int HISTORY_DEFAULT = 50, HISTORY_MAX = 100;
constexpr int EVENTS_MAX = 200;
constexpr int64_t EVENTS_KEEP_MS = 7LL * 24 * 3600 * 1000;
constexpr int WAIT_MS = 25 * 1000;       /* how long a request for events is held open */

/* What /pond/v1/info says this Pond speaks (docs/design.md, "Versions"). PROTOCOL counts additions within /pond/v1; a change
 * an older Puddle would misread gets a new path instead. FEATURES names the optional parts this Pond has, none yet. */
constexpr int PROTOCOL = 1;
constexpr std::array<const char *, 0> FEATURES{};

int64_t now_ms();

/* A channel name: 1 to 32 of a-z, 0-9 and '-'. */
bool channel_name_ok(const std::string &s);

/* A new invite token: 26 characters of base32. */
std::string new_token();

/* `s` as a JSON string, quotes included. */
std::string jstr(const std::string &s);

/* What a handler answers: an HTTP status, and a JSON body ("" for none). */
struct Answer {
    int status = 200;
    std::string body;
};
Answer fail(int status, const char *code);

/* A member's Puddle's key chain, checked (puddle_chain_verify). */
struct Chain {
    std::string id;
    std::string sign_pub_hex, kid;
    uint64_t seq = 0;
    std::string text;
};

class Service {
public:
    Service(Pool &pool, PondKey key);
    ~Service();
    const PondKey &key() const { return key_; }
    int wait_ms = WAIT_MS; /* how long events() holds a request; tests shorten it */

    /* ---- for the routes ---- */

    /* The current signing key of the member `id`, if `kid` names it (the request signature check). */
    bool member_key(const std::string &id, const std::string &kid, unsigned char pub[32]);
    bool is_member(const std::string &id);

    /* `profile` is the JSON object, or "" for none. */
    Answer join(const Chain &chain, const std::string &invite, const std::string &profile);
    Answer keys(const std::string &member, const Chain &chain);
    Answer info(const std::string &member);
    Answer history(const std::string &channel, int64_t before, int limit);
    Answer send(const std::string &member, const std::string &channel, const std::string &text, const std::string &nonce);
    /* Holds the caller until there is something after `after`, `WAIT_MS` pass, or stop(). */
    Answer events(int64_t after);
    Answer members();
    Answer set_profile(const std::string &member, const std::string &profile);
    Answer leave(const std::string &member);

    /* ---- for pondctl: false with the reason in `err` ---- */

    bool set_name(const std::string &name, const std::string *description, std::string &err);
    bool channel_create(const std::string &name, const std::string &topic, std::string &err);
    bool channel_rename(const std::string &name, const std::string &to, std::string &err);
    bool channel_topic(const std::string &name, const std::string &topic, std::string &err);
    bool channel_delete(const std::string &name, std::string &err);
    /* `max_uses` 0 = unlimited, `expires_ms` 0 = never. The token made is in `token`. */
    bool invite_create(int max_uses, int64_t expires_ms, std::string &token, std::string &err);
    bool invite_revoke(const std::string &token, std::string &err);
    bool member_remove(const std::string &id, std::string &err);
    std::string setting(const std::string &name);

    /* ---- the event log ---- */

    /* Watch the newest event number twice a second, so a change made by pondctl or another process wakes those waiting; and
     * forget events past EVENTS_KEEP_MS once an hour. Runs until stop(). */
    void watch();
    void stop();

private:
    Answer enter(const Chain &chain, const std::string &invite, const std::string &profile);
    bool add_event(Db &db, const char *kind, const std::string &data);
    void bump(int64_t id);
    bool live_channel(Db &db, const std::string &name_or_id, int64_t &id, std::string *json, bool by_name);
    std::string member_json(const std::vector<Cell> &row); /* id, profile, joined_ms */
    std::string pond_json(Db &db);
    std::string channels_json(Db &db);

    Pool &pool_;
    PondKey key_;
    puddle_keyed *send_limit_;
    std::mutex mu_;
    std::condition_variable cv_;
    int64_t latest_ = 0, oldest_ = 0;
    std::atomic<bool> stopping_{false};
};

} // namespace pond
