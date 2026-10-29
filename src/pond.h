#pragma once

/* What a Pond does, apart from HTTP: members joining and leaving, channels, messages, invites and the event log that members'
 * Puddles follow (docs/design.md). pondd's routes call it for members; pondctl calls it for the admin. Every answer is JSON
 * text, in the shapes api/pond.yaml gives. Everything that touches the database is a coroutine, awaited (co_await, or
 * drogon::sync_wait in pondctl). */

#include "db.h"
#include "key.h"
#include "ratelimit.h"

#include <drogon/utils/coroutine.h>
#include <trantor/net/EventLoop.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace pond {

using drogon::Task;

constexpr size_t TEXT_MAX = 4000;        /* characters in a message */
constexpr size_t TOPIC_MAX = 1024;
constexpr size_t POND_NAME_MAX = 100;    /* the Pond's name */
constexpr size_t DESCRIPTION_MAX = 1000;
constexpr size_t PROFILE_MAX = 16 * 1024;
constexpr int HISTORY_DEFAULT = 50, HISTORY_MAX = 100;
constexpr int EVENTS_MAX = 200;
constexpr int64_t EVENTS_KEEP_MS = 7LL * 24 * 3600 * 1000;

/* What /pond/v1/info says this Pond speaks (docs/design.md, "Versions"). PROTOCOL counts additions within /pond/v1; a change
 * an older Puddle would misread gets a new path instead. FEATURES names the optional parts this Pond has, none yet. */
constexpr int PROTOCOL = 1;
constexpr std::array<const char *, 0> FEATURES{};

int64_t now_ms();

/* A channel name: 1 to 32 of a-z, 0-9 and '-'. */
bool channel_name_ok(const std::string &s);

/* An invite token: 26 characters of base32. */
std::string new_token();
bool token_ok(const std::string &s);

/* What a handler answers: an HTTP status, and a JSON body ("" for none). */
struct Answer {
    int status = 200;
    std::string body;
};
Answer fail(int status, const char *code);

/* A member's Puddle's key chain, checked (verify_chain). */
struct Chain {
    std::string id;
    std::string sign_pub_hex, kid;
    uint64_t seq = 0;
    std::string text;
};

/* What an admin action did: "" if it worked, else why not. */
using Outcome = std::string;

class Service {
public:
    /* `timers` runs the waits for events and the watch; it must be running while those are used (pondd's main loop). */
    Service(DbPtr db, PondKey key, trantor::EventLoop *timers = nullptr);
    const PondKey &key() const { return key_; }
    int wait_ms = 25000; /* how long events() holds a request */

    /* ---- for the routes ---- */

    /* The current signing key of the member `id`, if `kid` names it (the request signature check). */
    Task<std::optional<std::array<unsigned char, 32>>> member_key(std::string id, std::string kid);
    Task<bool> is_member(std::string id);

    /* `profile` is the JSON object, or "" for none. */
    Task<Answer> join(Chain chain, std::string invite, std::string profile);
    Task<Answer> keys(std::string member, Chain chain);
    Task<Answer> info(std::string member);
    Task<Answer> history(int64_t channel, int64_t before, int limit);
    Task<Answer> send(std::string member, int64_t channel, std::string text, std::string nonce);
    /* Answers when there is something after `after`, `wait_ms` pass, or stop(). */
    Task<Answer> events(int64_t after);
    Task<Answer> members();
    Task<Answer> set_profile(std::string member, std::string profile);
    Task<Answer> leave(std::string member);

    /* ---- for pondctl ---- */

    Task<Outcome> set_name(std::string name, std::optional<std::string> description);
    Task<Outcome> channel_create(std::string name, std::string topic);
    Task<Outcome> channel_rename(std::string name, std::string to);
    Task<Outcome> channel_topic(std::string name, std::string topic);
    Task<Outcome> channel_delete(std::string name);
    /* `max_uses` 0 = unlimited, `expires_ms` 0 = never. The token made is in `token`. */
    Task<Outcome> invite_create(int max_uses, int64_t expires_ms, std::shared_ptr<std::string> token);
    Task<Outcome> invite_revoke(std::string token);
    Task<Outcome> member_remove(std::string id);
    Task<std::string> setting(std::string name);

    /* ---- the event log ---- */

    /* Check the newest event number twice a second, so a change made by pondctl or another process wakes those waiting; and
     * forget events past EVENTS_KEEP_MS once an hour. Runs on `timers` until stop(). */
    void watch();
    /* Answer everyone waiting, and stop watching. */
    void stop();

private:
    struct Waiter;
    struct News;
    Task<Answer> enter(Chain chain, std::string invite, std::string profile);
    Task<int64_t> add_event(const TxPtr &tx, const char *kind, const std::string &data);
    Task<std::string> member_json_of(const DbPtr &db, const std::string &id);
    Task<std::string> pond_json(const DbPtr &db);
    Task<std::string> channels_json();
    Task<Outcome> channel_set(std::string name, const char *column, std::string value);
    Task<Outcome> remove_member(std::string id);
    void bump(int64_t id);
    Task<> tick();

    DbPtr db_;
    PondKey key_;
    trantor::EventLoop *timers_;
    Keyed send_limit_{20, 200}; /* 5 a second on average, 20 at once */
    std::mutex mu_;
    std::vector<std::shared_ptr<Waiter>> waiters_;
    int64_t latest_ = 0, oldest_ = 0, next_prune_ = 0;
    std::atomic<bool> stopping_{false}, ticking_{false};
};

} // namespace pond
