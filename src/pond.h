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
constexpr std::array<const char *, 1> FEATURES{"files"};

/* Files (docs/design.md, "Files"): split into chunks of FILE_CHUNK bytes as a Puddle splits them, so a file has the same id in
 * a Pond as in a Puddle. How large a file may be is the admin's choice (`pondctl files max-size`), up to FILE_HARD_MAX. */
constexpr int64_t FILE_CHUNK = 1 << 20;
constexpr int64_t FILE_HARD_MAX = 4LL << 30;           /* 4096 chunks */
constexpr int64_t FILES_DEFAULT_MAX = 25LL << 20;
constexpr size_t FILES_PER_MESSAGE = 10;
constexpr size_t FILE_NAME_MAX = 255;                  /* bytes */
constexpr size_t FILE_MIME_MAX = 127;
constexpr size_t FILE_PREVIEW_MAX = 1400;              /* characters of base64: a preview of at most 1 KiB */
constexpr int64_t FILE_UNSENT_KEEP_MS = 24LL * 3600 * 1000; /* an upload no message carries goes after this */

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
    bool bytes = false; /* the body is a file's bytes (application/octet-stream), not JSON */
};
Answer fail(int status, const char *code);

/* A member's Puddle's key chain, checked (verify_chain). */
struct Chain {
    std::string id;
    std::string sign_pub_hex, kid;
    uint64_t seq = 0;
    std::string text;
};

/* A file as a message describes it: what the sender says of it. Its size is the Pond's own record. */
struct FileRef {
    std::string id, name, mime, preview;
    int64_t w = 0, h = 0, duration_ms = 0; /* 0 if not said */
};

/* What an admin action did: "" if it worked, else why not. */
using Outcome = std::string;

class Service {
public:
    /* `timers` runs the waits for events and the watch; it must be running while those are used (pondd's main loop). */
    Service(DbPtr db, PondKey key, std::string data_dir, trantor::EventLoop *timers = nullptr);
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
    Task<Answer> send(std::string member, int64_t channel, std::string text, std::string nonce, std::vector<FileRef> files = {});
    /* Answers when there is something after `after`, `wait_ms` pass, or stop(). */
    Task<Answer> events(int64_t after);
    Task<Answer> members();
    Task<Answer> set_profile(std::string member, std::string profile);
    Task<Answer> leave(std::string member);

    /* Files (files.cpp). Begin one with its size and chunk hashes (32 bytes each): answers which chunks are still wanted. */
    Task<Answer> file_begin(std::string member, std::string id, int64_t size, std::string hashes);
    Task<Answer> file_put(std::string member, std::string id, int64_t n, std::string bytes);
    /* Its chunk hashes, or chunk `n`: to a member, of a whole file a message in a live channel carries (or they sent). */
    Task<Answer> file_chunks(std::string member, std::string id);
    Task<Answer> file_chunk(std::string member, std::string id, int64_t n);
    /* The largest file members may send (0: files are turned off). */
    Task<int64_t> files_max();

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
    /* `bytes` 0 turns files off. */
    Task<Outcome> set_files_max(int64_t bytes);
    /* How many files are kept, and the bytes they take. */
    Task<std::pair<int64_t, int64_t>> files_usage();

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
    Task<> sweep_files();
    Task<bool> may_read(const std::string &member, const std::string &id);
    Task<std::string> messages_json(const DbPtr &db, const drogon::orm::Result &r);
    std::string file_path(const std::string &id, bool partial) const;

    DbPtr db_;
    PondKey key_;
    std::string files_dir_;
    trantor::EventLoop *timers_;
    Keyed send_limit_{20, 200}; /* 5 a second on average, 20 at once */
    Keyed chunk_limit_{64, 50}; /* chunks sent or fetched: 20 MiB a second on average, 64 at once */
    std::mutex mu_;
    std::vector<std::shared_ptr<Waiter>> waiters_;
    int64_t latest_ = 0, oldest_ = 0, next_prune_ = 0;
    std::atomic<bool> stopping_{false}, ticking_{false};
};

} // namespace pond
