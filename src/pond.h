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
#include <coroutine>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
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
 * an older Puddle would misread gets a new path instead. FEATURES names the optional parts this Pond has. */
constexpr int PROTOCOL = 8; /* 2: roles (docs/design.md, "Roles"); 3: receipts, and the Pond's key in `pond` ("Receipts");
                             * 4: channels' `kind`, and voice channels (docs/voice.md) where `features` says `voice`;
                             * 5: sealed requests and answers, receipts v2, and `seal_key` in `pond` (the protocol's spec/09);
                             * 6: pictures, the Pond's `icon` and members' profile pictures ("Pictures");
                             * 7: muting, and a member's `muted` ("Muting");
                             * 8: direct messages, relayed (docs/dms.md), where `features` says `dms`; a member's `dms` */
constexpr std::array<const char *, 5> FEATURES{"files", "roles", "receipts", "sealed", "pictures"}; /* and "voice", when voice is on */

/* Direct messages (docs/dms.md, dms.cpp): relayed, never stored. */
constexpr size_t DM_SEALED_MAX = 16 * 1024;            /* a sealed letter, as base64 */
constexpr int64_t DM_CHUNK = 1 << 20;                  /* a piece of a file: a Puddle's chunk */
constexpr size_t DM_PIECE_MAX = DM_CHUNK + 64;         /* sealed */
constexpr int64_t DM_FILES_DEFAULT_MAX = 25LL << 20;
constexpr int64_t DM_FILES_HARD_MAX = 4LL << 30;
constexpr int64_t DM_PRESENT_MS = 60 * 1000;           /* a Puddle that asked for events this recently is following */
constexpr size_t DM_INBOX_MAX = 500;                   /* waiting to go to one member's Puddle, in memory */
constexpr size_t DM_AWAITING_MAX = 1000;               /* senders waiting for one member to be back */
constexpr size_t DM_NEW_PER_DAY = 20;                  /* new people one member may write to in a day */
constexpr size_t DM_PAIRS_MAX = 1 << 20;
constexpr size_t DM_PIECES_MAX = 4096;                 /* pieces asked for and not yet sent, across the Pond */
constexpr double DM_PIECE_WAIT_S = 30;

/* Roles (docs/design.md, "Roles"), lowest first. An operator outranks an administrator. */
enum class Role { MEMBER = 0, ADMINISTRATOR = 1, OPERATOR = 2 };
const char *role_name(Role r);
std::optional<Role> role_of_name(const std::string &s);
constexpr int64_t OFFER_KEEP_MS = 7LL * 24 * 3600 * 1000;
constexpr size_t BAN_REASON_MAX = 500;
constexpr int INVITE_USES_MAX = 1000000;
constexpr int64_t INVITE_EXPIRY_MAX_MS = 10LL * 366 * 24 * 3600 * 1000; /* made from the app: at most ten years ahead */

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

/* Pictures (docs/design.md, "Pictures"): the icon and members' profile pictures, kept by their SHA-256 (pictures.cpp). */
constexpr size_t PICTURE_MAX = 1 << 20;
/* "image/png", "image/jpeg", "image/gif" or "image/webp" from what the bytes start with; nullptr for anything else */
const char *picture_kind(std::string_view bytes);
bool picture_hash_ok(const std::string &hash);

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
    /* host[:port] invite links name (pond.yaml's `address`): set by pondd and pondctl */
    std::string link_address;
    /* The link to give out for an invite: pond://<address>?invite=<token>&k=<the sealing key> */
    std::string invite_link(const std::string &token) const;

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
    Task<Answer> events(int64_t after, std::string member);
    Task<Answer> members();
    Task<Answer> set_profile(std::string member, std::string profile);
    Task<Answer> leave(std::string member);

    /* Files (files.cpp). Begin one with its size and chunk hashes (32 bytes each): answers which chunks are still wanted. */
    Task<Answer> file_begin(std::string member, std::string id, int64_t size, std::string hashes);
    Task<Answer> file_put(std::string member, std::string id, int64_t n, std::string bytes);
    /* Its chunk hashes, or chunk `n`: to a member, of a whole file a message in a live channel carries (or they sent). */
    Task<Answer> file_chunks(std::string member, std::string id);
    Task<Answer> file_chunk(std::string member, std::string id, int64_t n);
    /* Roles (roles.cpp). Answer the open offer `offer` made to `member`. */
    Task<Answer> answer_offer(std::string member, int64_t offer, bool accept);
    /* `member`'s role; MEMBER for someone who is not one. */
    Task<Role> role(std::string member);
    /* What an operator or administrator does from the app; `by` is checked to hold a role first (the routes). */
    Task<Answer> admin_pond(std::string by, std::optional<std::string> name, std::optional<std::string> description);
    Task<Answer> admin_channel_create(std::string by, std::string name, std::string topic, std::string kind = "text");
    Task<Answer> admin_channel_edit(std::string by, int64_t channel, std::optional<std::string> name, std::optional<std::string> topic);
    Task<Answer> admin_channel_delete(std::string by, int64_t channel);
    Task<Answer> admin_remove(std::string by, std::string member, bool ban, std::string reason);
    Task<Answer> admin_bans();
    /* Mute `member` (docs/design.md, "Muting"): until `until_ms`, or until unmuted (0); or unmute them. */
    Task<Answer> admin_mute(std::string by, std::string member, int64_t until_ms);
    Task<Answer> admin_unmute(std::string by, std::string member);
    /* Is `member` muted now? -1 if not; 0 until unmuted; else until when (ms). From memory, so voice may ask for every frame. */
    int64_t muted_until(const std::string &member);
    /* Invites from the app (docs/design.md, "Invite links"): those that can still be used; one made with `max_uses` (0: no
     * limit) and `expires_ms` (0: never); one revoked. */
    Task<Answer> admin_invites();
    Task<Answer> admin_invite_create(std::string by, int max_uses, int64_t expires_ms);
    Task<Answer> admin_invite_revoke(std::string by, std::string token);
    Task<Answer> admin_unban(std::string by, std::string member);

    /* Pictures (pictures.cpp): keep one a member sends, answering {hash, mime, size}; give one that is shown to the members (the
     * icon, or in a member's profile) to a member. */
    Task<Answer> picture_put(std::string member, std::string bytes);
    Task<Answer> picture_get(std::string member, std::string hash);
    /* The Pond's icon: a picture sent first, or nullopt for none. */
    Task<Answer> admin_icon(std::string by, std::optional<std::string> hash);

    /* Direct messages (dms.cpp, docs/dms.md). A member's chain, for sealing to them; a letter relayed (or its sender told the
     * recipient isn't there); whether `member` takes them; a piece of a letter's file, asked for by its recipient and sent by its
     * sender. */
    Task<Answer> member_chain(std::string member);
    Task<Answer> dm_send(std::string from, std::string to, std::string id, std::string kid, std::string enc, std::string sealed);
    Task<Answer> dm_accept(std::string member, bool accept);
    Task<Answer> dm_piece_get(std::string to, std::string from, std::string letter, int n, std::string part);
    Task<Answer> dm_piece_put(std::string from, std::string to, std::string letter, int n, std::string part, std::string bytes);
    struct DmSettings {
        bool on = true;
        int64_t files_max = DM_FILES_DEFAULT_MAX; /* 0: no files */
    };
    Task<DmSettings> dm_settings();
    /* pondctl dms: on or off for the whole Pond, and the largest file (0: none) */
    Task<Outcome> set_dms(std::optional<bool> on, std::optional<int64_t> files_max);

    /* The largest file members may send (0: files are turned off). */
    Task<int64_t> files_max();

    /* ---- for pondctl ---- */

    Task<Outcome> set_name(std::string name, std::optional<std::string> description);
    /* `kind`: "text" or "voice" (only while voice is on) */
    Task<Outcome> channel_create(std::string name, std::string topic, std::string kind = "text");
    Task<Outcome> channel_rename(std::string name, std::string to);
    Task<Outcome> channel_topic(std::string name, std::string topic);
    Task<Outcome> channel_delete(std::string name);
    /* `max_uses` 0 = unlimited, `expires_ms` 0 = never. The token made is in `token`. */
    Task<Outcome> invite_create(int max_uses, int64_t expires_ms, std::shared_ptr<std::string> token);
    Task<Outcome> invite_revoke(std::string token);
    Task<Outcome> member_remove(std::string id);
    /* Offer `role` to the member `id` (replacing an open offer); they accept it in their app. */
    Task<Outcome> offer_role(std::string id, Role role);
    /* Set their role at once: for taking one away (or giving one without asking, which pondctl does not offer). */
    Task<Outcome> set_role(std::string id, Role role, std::string by);
    Task<Outcome> ban(std::string id, std::string reason, std::string by);
    Task<Outcome> unban(std::string id, std::string by);
    /* Record an admin action. */
    Task<> log_admin(std::string by, std::string action, std::string target, std::string detail);
    Task<std::string> setting(std::string name);
    /* Keep a picture (pondctl's `icon`; picture_put for members): its hash in `hash`. */
    Outcome store_picture(const std::string &bytes, std::string &hash);
    /* The icon: the hash of a picture kept, or nullopt to have none. */
    Task<Outcome> set_icon(std::optional<std::string> hash);
    /* `bytes` 0 turns files off. */
    Task<Outcome> set_files_max(int64_t bytes);
    /* How many files are kept, and the bytes they take. */
    Task<std::pair<int64_t, int64_t>> files_usage();

    /* ---- voice channels (voice.h) ---- */

    /* Voice channels may be made (pondd: built with voice, and `voice` not false). */
    bool voice_on = false;
    /* Who is in which voice channel now, as a JSON array for /info; set by pondd while voice is on. */
    std::function<std::string()> voice_now;
    /* Tell every member: `voice` or `voice_left` with its data. */
    Task<> voice_event(std::string kind, std::string data);
    /* Of `members`, those no longer members; of `channels`, those that are no longer live voice channels. */
    Task<std::pair<std::vector<std::string>, std::vector<int64_t>>> voice_gone(std::vector<std::string> members, std::vector<int64_t> channels);
    /* `channel` is a live voice channel */
    Task<bool> is_voice_channel(int64_t channel);

    /* ---- the event log ---- */

    /* Check the newest event number twice a second, so a change made by pondctl or another process wakes those waiting; and
     * forget events past EVENTS_KEEP_MS once an hour. Runs on `timers` until stop(). */
    void watch();
    /* Answer everyone waiting, and stop watching. */
    void stop();

private:
    /* One request for events, waiting. Whoever sets `done` first (news, the time running out, or stopping) resumes it. */
    struct Waiter {
        int64_t after;
        std::string member; /* whose request: a direct message for them wakes it (dms.cpp) */
        std::coroutine_handle<> h;
        std::atomic<bool> done{false};
        void wake()
        {
            if (!done.exchange(true))
                h.resume();
        }
    };
    struct News;
    struct Piece;
    struct Following;
    struct PieceWait;
    /* direct messages, in memory (dms.cpp); all under mu_ */
    struct Seen {
        int open = 0;        /* requests for events held open now */
        int64_t last_ms = 0; /* the last one began or ended */
    };
    struct NewToday {
        int64_t day = 0;
        std::set<std::string> to;
    };
    std::map<std::string, Seen> seen_;
    std::map<std::string, std::deque<std::string>> inbox_;       /* member: {kind, data} for them alone, not yet taken */
    std::map<std::string, std::set<std::string>> awaiting_;      /* member: who wants to write to them when they are back */
    std::map<std::string, std::shared_ptr<Piece>> pieces_;
    std::map<std::string, NewToday> new_today_;
    std::set<std::string> wrote_;                                /* "<from> <to>": has written to them */
    Keyed dm_limit_{10, 1000};   /* letters: one a second on average, ten at once */
    Keyed piece_limit_{64, 50};  /* pieces asked for */
    bool present_locked(const std::string &member, int64_t now) const;
    bool has_inbox_locked(const std::string &member) const;
    void push_locked(const std::string &member, const std::string &kind, const std::string &data, std::vector<std::shared_ptr<Waiter>> &wake);
    /* a request for events from `member` begins (open) or ends */
    void following(const std::string &member, bool open);
    std::vector<std::string> take_inbox(const std::string &member);
    void forget_dms(const std::string &member);
    Task<std::string> may_write(std::string from, std::string to);
    Task<Answer> enter(Chain chain, std::string invite, std::string profile);
    Task<int64_t> add_event(TxPtr tx, const char *kind, std::string data, std::string only_for = "");
    Task<std::string> offer_json(std::string member);
    /* May `by` act on `target`? "" if so, else the error code. */
    Task<std::string> may_act(std::string by, std::string target);
    Task<std::string> member_json_of(DbPtr db, std::string id);
    Task<std::string> pond_json(DbPtr db);
    Task<std::string> channels_json();
    Task<Outcome> channel_set(std::string name, const char *column, std::string value);
    Task<Outcome> remove_member(std::string id);
    void bump(int64_t id);
    Task<> tick();
    Task<> sweep_files();
    Task<> sweep_pictures();
    Task<bool> picture_shown(std::string hash);
    std::string picture_path(const std::string &hash) const;
    /* {hash, mime, size} of a picture kept, into `json`: false if it is not here */
    bool picture_ref(const std::string &hash, std::string &json) const;
    Task<bool> may_read(std::string member, std::string id);
    Task<std::string> messages_json(DbPtr db, drogon::orm::Result r);
    std::string file_path(const std::string &id, bool partial) const;

    DbPtr db_;
    PondKey key_;
    std::string files_dir_, pictures_dir_;
    trantor::EventLoop *timers_;
    Keyed send_limit_{20, 200}; /* 5 a second on average, 20 at once */
    Keyed chunk_limit_{64, 50}; /* chunks sent or fetched: 20 MiB a second on average, 64 at once */
    std::mutex mu_;
    std::vector<std::shared_ptr<Waiter>> waiters_;
    int64_t latest_ = 0, oldest_ = 0, next_prune_ = 0, next_mutes_ = 0;
    std::map<std::string, int64_t> muted_; /* who is muted: muted_until as kept; reloaded every two seconds by tick() */
    Task<Outcome> set_muted(std::string member, std::optional<int64_t> until);
    std::atomic<bool> stopping_{false}, ticking_{false};
};

} // namespace pond
