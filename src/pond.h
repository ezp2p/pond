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
constexpr int PROTOCOL = 14; /* 2: roles (docs/design.md, "Roles"); 3: receipts, and the Pond's key in `pond` ("Receipts");
                             * 4: channels' `kind`, and voice channels (docs/voice.md) where `features` says `voice`;
                             * 5: sealed requests and answers, receipts v2, and `seal_key` in `pond` (the protocol's spec/09);
                             * 6: pictures, the Pond's `icon` and members' profile pictures ("Pictures");
                             * 7: muting, and a member's `muted` ("Muting");
                             * 8: direct messages, relayed (docs/dms.md), where `features` says `dms`; a member's `dms`;
                             * 9: roles for channels, and the channels' order from the app ("Roles for channels"): `roles` in
                             *    /info, a member's and a channel's `roles`, and the events `role`, `role_deleted` and `channels`;
                             * 10: what roles let their members do, their colour, order, and whether they are shown apart ("Permissions"):
                             *    a role's `color`, `hoist` and `permissions`, the order of `roles` in /info, and the event `role_order`;
                             * 11: pages, and the Pond's banner ("Pages"): `pages` in /info, GET /pond/v1/pages/{page}, the `banner` in
                             *    `pond`, the permission `manage_pages`, and the events `page`, `page_deleted` and `page_order`;
                             * 12: names in a role's colour ("Role colours"): a member's `role_color`, `role_colors` in `pond`,
                             *    POST /pond/v1/role_color and /pond/v1/admin/role_colors;
                             * 13: permissions for everyone and for each channel ("Permissions"): `everyone` in /info, a channel's
                             *    `overrides`, the permissions view_channels to manage_shop; pages for some roles, their media and their
                             *    kinds `roles`, `events`, `form` and `shop` ("Pages"); applying to join ("Entry form"); the shop and its
                             *    payments ("Shop"); and moderating voice ("Voice moderation");
                             * 14: editing and deleting messages, and the message log ("Editing and deleting messages"): a message's
                             *    `edited`, the events `message_edited` and `message_deleted`, the permissions manage_messages and
                             *    view_message_log, and the routes under /pond/v1/channels/{channel}/messages/{message} and .../log */
constexpr std::array<const char *, 15> FEATURES{"files", "roles", "receipts", "sealed", "pictures", "channel_roles", "role_permissions", "pages",
                                                "role_colors", "overrides", "page_kinds", "applications", "shop", "voice_moderation",
                                                "message_edits"}; /* and "voice", when voice is on */

/* Direct messages (docs/dms.md, dms.cpp): relayed, never stored. */
constexpr size_t DM_SEALED_MAX = 128 * 1024;           /* a sealed letter, as base64: a message with ten files' previews fits */
constexpr int64_t DM_CHUNK = 1 << 20;                  /* a piece of a file: a Puddle's chunk */
constexpr size_t DM_PIECE_MAX = DM_CHUNK + 1024;       /* sealed, as records of 64 KiB */
constexpr int64_t DM_FILES_DEFAULT_MAX = 25LL << 20;
constexpr int64_t DM_FILES_HARD_MAX = 4LL << 30;
constexpr int64_t DM_PRESENT_MS = 15 * 1000;           /* a Puddle that asked for events this recently is following */
constexpr int JOIN_TRIES = 5;                            /* a join that met others at the same moment is tried this often */
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
constexpr size_t ROLE_NAME_MAX = 32;   /* characters in the name of a role for channels */
constexpr int64_t ROLES_MAX = 200;     /* roles for channels in one Pond */

/* What a role lets the members who hold it do (docs/design.md, "Permissions"): bits in `roles.permissions`, names on the wire.
 * An operator or administrator may do all of it. */
enum Permission : int64_t {
    MANAGE_POND = 1, MANAGE_CHANNELS = 2, MANAGE_ROLES = 4, MANAGE_INVITES = 8, KICK_MEMBERS = 16, BAN_MEMBERS = 32, MUTE_MEMBERS = 64,
    MANAGE_PAGES = 128,
    /* protocol 13: what everyone may do unless a channel says otherwise (docs/design.md, "Permissions") */
    VIEW_CHANNELS = 256, SEND_MESSAGES = 512, ATTACH_FILES = 1024, CONNECT = 2048, SPEAK = 4096, VIDEO = 8192,
    MOVE_MEMBERS = 16384, VOICE_MUTE = 32768, MANAGE_EVENTS = 65536, MANAGE_APPLICATIONS = 131072, MANAGE_SHOP = 262144,
    /* protocol 14: deleting others' messages, and reading what messages said before they were edited or deleted
     * (docs/design.md, "Editing and deleting messages") */
    MANAGE_MESSAGES = 524288, VIEW_MESSAGE_LOG = 1048576
};
constexpr int64_t ALL_PERMISSIONS = (1 << 21) - 1;
/* those a channel may allow or deny a role (its `overrides`) */
constexpr int64_t CHANNEL_PERMISSIONS = VIEW_CHANNELS | SEND_MESSAGES | ATTACH_FILES | CONNECT | SPEAK | VIDEO | MOVE_MEMBERS | VOICE_MUTE |
                                        MANAGE_MESSAGES | VIEW_MESSAGE_LOG;
/* what everyone has in a Pond that has said nothing else: what members could always do */
constexpr int64_t EVERYONE_DEFAULT = VIEW_CHANNELS | SEND_MESSAGES | ATTACH_FILES | CONNECT | SPEAK | VIDEO;
struct PermissionName {
    int64_t bit;
    const char *name;
};
constexpr std::array<PermissionName, 21> PERMISSIONS{{{MANAGE_POND, "manage_pond"}, {MANAGE_CHANNELS, "manage_channels"},
                                                      {MANAGE_ROLES, "manage_roles"}, {MANAGE_INVITES, "manage_invites"},
                                                      {KICK_MEMBERS, "kick_members"}, {BAN_MEMBERS, "ban_members"},
                                                      {MUTE_MEMBERS, "mute_members"}, {MANAGE_PAGES, "manage_pages"},
                                                      {VIEW_CHANNELS, "view_channels"}, {SEND_MESSAGES, "send_messages"},
                                                      {ATTACH_FILES, "attach_files"}, {CONNECT, "connect"}, {SPEAK, "speak"},
                                                      {VIDEO, "video"}, {MOVE_MEMBERS, "move_members"}, {VOICE_MUTE, "voice_mute"},
                                                      {MANAGE_EVENTS, "manage_events"}, {MANAGE_APPLICATIONS, "manage_applications"},
                                                      {MANAGE_SHOP, "manage_shop"}, {MANAGE_MESSAGES, "manage_messages"},
                                                      {VIEW_MESSAGE_LOG, "view_message_log"}}};
/* A permission's bit from its name; 0 for none */
int64_t permission_bit(const std::string &name);
/* What a channel allows and denies one role (0: everyone) */
struct Override {
    int64_t role = 0, allow = 0, deny = 0;
};

/* Pages (docs/design.md, "Pages"): what a Pond shows above its channels. A page has a kind; "markdown" is the only one yet,
 * and the others (roles to pick, events, an entry form, a shop) will each be one. */
constexpr int64_t PAGES_MAX = 50;
constexpr size_t PAGE_TITLE_MAX = 64;          /* characters */
constexpr size_t PAGE_BODY_MAX = 64 * 1024;    /* characters of markdown */
constexpr size_t PAGE_FILES_MAX = 10;         /* as many as a message: a Puddle sends no more at once */
constexpr std::array<const char *, 5> PAGE_KINDS{"markdown", "roles", "events", "form", "shop"};
constexpr size_t PAGE_DATA_MAX = 64 * 1024;    /* a kind's settings, as JSON */
constexpr size_t PAGE_OPTIONS_MAX = 50;        /* roles one page offers */
constexpr size_t FORM_QUESTIONS_MAX = 30;
constexpr size_t FORM_TEXT_MAX = 500;          /* a question, a choice, a short answer */
constexpr size_t FORM_ANSWER_MAX = 4000;       /* a long answer */
constexpr size_t EVENT_TITLE_MAX = 100, EVENT_TEXT_MAX = 4000, EVENT_PLACE_MAX = 200;
constexpr int64_t EVENTS_PER_PAGE_MAX = 500;
constexpr size_t ITEM_TITLE_MAX = 100, ITEM_TEXT_MAX = 4000;
constexpr int64_t ITEMS_PER_PAGE_MAX = 200;
constexpr int64_t PRICE_MAX = 99999999;        /* in the currency's smallest unit: a Stripe limit */
constexpr int64_t ORDER_KEEP_OPEN_MS = 24LL * 3600 * 1000; /* an order not paid in this long lapses */
std::string permissions_json(int64_t bits);
/* A role on the wire: {id, name, color (null or "#rrggbb"), hoist, permissions} */
std::string role_json(int64_t id, const std::string &name, const std::string &color, bool hoist, int64_t permissions);
struct Override;
std::vector<Override> parse_overrides(const std::string &concat);
std::string overrides_json(const std::vector<Override> &list);

/* What a member may do: their rank, what the roles they hold let them do, and the highest of those roles (its `position`:
 * the lower, the higher; INT64_MAX for none). */
struct Powers {
    Role rank = Role::MEMBER;
    int64_t permissions = 0;
    int64_t top = INT64_MAX;
    bool ranked() const { return rank != Role::MEMBER; }
    bool has(int64_t p) const { return ranked() || (permissions & p) == p; }
};

/* A role made or changed from the app: what is to be set. `color` "" for none. */
struct RoleEdit {
    std::optional<std::string> name, color;
    std::optional<bool> hoist;
    std::optional<int64_t> permissions;
};
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
/* A file as a message or page carries it: its name cleaned, its type checked. false if it can't be. */
bool clean_file(FileRef &f);

/* A page made or changed from the app: what is to be set */
struct PageEdit {
    std::optional<std::string> title, kind, body;
    std::optional<std::vector<FileRef>> files;
    std::optional<std::string> data;                         /* the kind's settings, a JSON object */
    std::optional<std::optional<std::vector<int64_t>>> roles; /* who sees it: everyone (nullopt), or these roles */
};
/* An event on an events page (pages.cpp, "Events") */
struct EventEdit {
    std::optional<std::string> title, description, place;
    std::optional<int64_t> starts, ends; /* ms; `ends` 0 for none */
};
/* Something a shop page sells (shop.cpp) */
struct ItemEdit {
    std::optional<std::string> title, description, image; /* `image`: the name of one of the page's media, "" for none */
    std::optional<int64_t> price;                          /* in the currency's smallest unit; the least, for one paid as wished */
    std::optional<bool> custom;                            /* the buyer says how much (a donation) */
    std::optional<std::optional<int64_t>> stock;           /* nullopt: no limit */
    std::optional<bool> active;
};
/* Where the shop talks to Stripe: pond.yaml's `shop.stripe_api`, for tests */
struct ShopSettings {
    std::string stripe_api = "https://api.stripe.com";
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
    Task<Answer> history(std::string member, int64_t channel, int64_t before, int limit);
    Task<Answer> send(std::string member, int64_t channel, std::string text, std::string nonce, std::vector<FileRef> files = {});
    /* Editing and deleting messages (messages.cpp; docs/design.md, "Editing and deleting messages"). Its author changes its text
     * and drops some of its files (`keep`: the ids of those it keeps, in its order); its author, or whoever may manage messages in
     * its channel, deletes it. */
    Task<Answer> message_edit(std::string member, int64_t channel, int64_t message, std::optional<std::string> text,
                              std::optional<std::vector<std::string>> keep);
    Task<Answer> message_delete(std::string member, int64_t channel, int64_t message);
    /* The message log, for whoever may view it in the channel: its messages that were edited or deleted, newest first, each with
     * every text it had; or one message, edited or not. */
    Task<Answer> message_log(std::string member, int64_t channel, int64_t before, int limit);
    Task<Answer> message_versions(std::string member, int64_t channel, int64_t message);
    /* Erase a deleted message for good, with its versions, and its files that nothing else carries: for whoever may both manage
     * messages and view the log there. */
    Task<Answer> message_purge(std::string member, int64_t channel, int64_t message);
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
    /* What `member` may do (channel_roles.cpp). */
    Task<Powers> powers(std::string member);
    /* What an operator or administrator, or a member whose roles allow it, does from the app; `by` is checked to have the
     * permission first (the routes), and rank and the order of roles here. */
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
    /* Roles for channels (channel_roles.cpp; docs/design.md, "Roles for channels"): named by operators and administrators,
     * given to members, and asked for by channels that are not open to everyone. */
    Task<Answer> admin_role_create(std::string by, RoleEdit e);
    Task<Answer> admin_role_edit(std::string by, int64_t role, RoleEdit e);
    /* Pages (pages.cpp; docs/design.md, "Pages"): every page, in order, as /info lists them; one, whole, for a member; made,
     * changed, deleted and ordered by whoever may manage pages. */
    Task<std::string> pages_json();
    Task<Answer> page_get(std::string member, int64_t page);
    Task<Answer> admin_page_create(std::string by, PageEdit edit);
    Task<Answer> admin_page_edit(std::string by, int64_t page, PageEdit edit);
    Task<Answer> admin_page_delete(std::string by, int64_t page);
    Task<Answer> admin_page_order(std::string by, std::vector<int64_t> pages);
    /* The pages `member` sees, in order (a page for some roles only is for operators, administrators and those roles) */
    Task<std::string> pages_json_for(std::string member);
    Task<std::string> page_extra(std::string member, int64_t page, std::string kind, std::string data);
    Task<int64_t> page_changed(TxPtr tx, int64_t page, std::string title, std::string kind, int64_t updated);
    Task<int64_t> page_touched(TxPtr tx, int64_t page, std::string by);
    /* ",\"items\":[...],\"shop\":{...}" for a shop page, as `member` sees it */
    Task<std::string> shop_items_json(int64_t page, std::string member);
    Task<bool> may_see_page(std::string member, int64_t page);
    /* A roles page (docs/design.md, "Pages"): of the roles it offers, those `member` holds now */
    Task<Answer> page_pick_roles(std::string member, int64_t page, std::vector<int64_t> roles);
    /* An events page: events made, changed and deleted by whoever may manage events; and who is going */
    Task<Answer> admin_event_create(std::string by, int64_t page, EventEdit e);
    Task<Answer> admin_event_edit(std::string by, int64_t page, int64_t event, EventEdit e);
    Task<Answer> admin_event_delete(std::string by, int64_t page, int64_t event);
    Task<Answer> event_going(std::string member, int64_t page, int64_t event, bool going);
    /* A shop page (shop.cpp; docs/design.md, "Shop"): its items, and buying one through Stripe */
    Task<Answer> admin_item_create(std::string by, int64_t page, ItemEdit e);
    Task<Answer> admin_item_edit(std::string by, int64_t page, int64_t item, ItemEdit e);
    Task<Answer> admin_item_delete(std::string by, int64_t page, int64_t item);
    Task<Answer> shop_buy(std::string member, int64_t page, int64_t item, int64_t quantity, int64_t amount);
    /* a member's own orders, or (`all`) every order, for whoever may manage the shop */
    Task<Answer> orders(std::string member, bool all);
    Task<Answer> admin_stripe(std::string by, std::optional<std::string> key);
    ShopSettings shop_settings;
    /* An entry form (applications.cpp; docs/design.md, "Entry form"): a stranger applies with their answers; whoever may manage
     * applications sees them and accepts (they become a member) or declines */
    Task<Answer> apply(Chain chain, std::string invite, std::string profile, std::string answers);
    Task<Answer> admin_applications();
    Task<Answer> admin_application(std::string by, std::string member, bool accept, std::string reason);
    /* May `by` act on `target`? "" if so, else the error code (rank and the order of roles) */
    Task<std::string> may_act(std::string by, std::string target);
    /* May `by` moderate `target` in voice channel `channel` with permission `need`? "" if so; "no_permission", or why not */
    Task<std::string> may_moderate(std::string by, std::string target, int64_t channel, int64_t need);
    /* the order of the roles: every role's id, highest first */
    Task<Answer> admin_role_order(std::string by, std::vector<int64_t> roles);
    Task<Answer> admin_role_delete(std::string by, int64_t role);
    /* the roles `member` holds now: these, and no others */
    Task<Answer> admin_member_roles(std::string by, std::string member, std::vector<int64_t> roles);
    /* who may see `channel`: everyone (nullopt), or operators, administrators and members with one of `roles` */
    Task<Answer> admin_channel_roles(std::string by, int64_t channel, std::optional<std::vector<int64_t>> roles);
    /* the order of the channels: every live channel's id, first to last */
    Task<Answer> admin_channel_order(std::string by, std::vector<int64_t> channels);
    /* What everyone may do (the Pond's `everyone`); what `member` may do in each live channel, or one (docs/design.md,
     * "Permissions", access.cpp) */
    Task<int64_t> everyone_permissions();
    Task<std::map<int64_t, int64_t>> channel_permissions(std::string member);
    Task<int64_t> permissions_in(std::string member, int64_t channel);
    Task<Answer> admin_everyone(std::string by, int64_t permissions);
    Task<Answer> admin_channel_overrides(std::string by, int64_t channel, std::vector<Override> overrides);
    /* The live channels `member` may not see; may they see `channel`? */
    Task<std::set<int64_t>> hidden_channels(std::string member);
    Task<bool> may_see(std::string member, int64_t channel);

    /* Pictures (pictures.cpp): keep one a member sends, answering {hash, mime, size}; give one that is shown to the members (the
     * icon, or in a member's profile) to a member. */
    Task<Answer> picture_put(std::string member, std::string bytes);
    Task<Answer> picture_get(std::string member, std::string hash);
    /* The Pond's icon: a picture sent first, or nullopt for none. */
    /* ...or its banner (`which` "banner": the picture over its name in the channel list; docs/design.md, "Pages") */
    Task<Answer> admin_icon(std::string by, std::optional<std::string> hash, std::string which = "icon");

    /* Direct messages (dms.cpp, docs/dms.md). A member's chain, for sealing to them; a letter relayed (or its sender told the
     * recipient isn't there); whether `member` takes them; a piece of a letter's file, asked for by its recipient and sent by its
     * sender. */
    Task<Answer> member_chain(std::string member);
    Task<Answer> dm_send(std::string from, std::string to, std::string id, std::string kid, std::string enc, std::string sealed);
    Task<Answer> dm_accept(std::string member, bool accept);
    /* Role colours (docs/design.md, "Role colours"): whether a member's name shows in their highest coloured role's colour, as
     * they choose; and whether the Pond makes everyone's do (`enforced`, set by those who may manage it). */
    Task<Answer> role_color(std::string member, bool use);
    Task<Answer> admin_role_colors(std::string by, bool enforced);
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
    Task<Outcome> set_icon(std::optional<std::string> hash, std::string which = "icon");
    /* `bytes` 0 turns files off. */
    Task<Outcome> set_files_max(int64_t bytes);
    /* pondctl message show|delete|purge: a message, with every text it had and where its files are on disk; deleted as a
     * moderator would; or erased for good (deleted first if it was not) */
    Task<Outcome> message_show(int64_t message, std::shared_ptr<std::string> out);
    Task<Outcome> message_delete_ctl(int64_t message);
    Task<Outcome> message_purge_ctl(int64_t message);
    /* How many files are kept, and the bytes they take. */
    Task<std::pair<int64_t, int64_t>> files_usage();

    /* ---- voice channels (voice.h) ---- */

    /* Voice channels may be made (pondd: built with voice, and `voice` not false). */
    bool voice_on = false;
    /* Who is in which voice channel now, as a JSON array for /info, leaving out the channels given; set by pondd while voice is on. */
    std::function<std::string(const std::set<int64_t> &)> voice_now;
    /* Tell every member: `voice` or `voice_left` with its data. */
    Task<> voice_event(std::string kind, std::string data);
    /* Of these (member, voice channel), those where they may no longer be: no longer a member, the channel gone, or no longer
     * one they may see. */
    Task<std::vector<std::pair<std::string, int64_t>>> voice_gone(std::vector<std::pair<std::string, int64_t>> in);
    /* `channel` is a live voice channel `member` may see and connect to */
    Task<bool> is_voice_channel(int64_t channel, std::string member);
    /* What each (member, voice channel) may do there now (0: they may no longer be there), for the voice checks */
    Task<std::vector<int64_t>> voice_rights(std::vector<std::pair<std::string, int64_t>> in);

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
    Task<Answer> enter_once(Chain chain, std::string invite, std::string profile); /* status 0: met another join, try again */
    /* `channel`: the event is about that channel, and only those who may see it are given it */
    Task<int64_t> add_event(TxPtr tx, const char *kind, std::string data, std::string only_for = "", int64_t channel = 0,
                            int64_t message = 0);
    /* messages.cpp: delete `message` (by `by`, a member or "pondctl"), rewriting its earlier events; erase it */
    Task<Answer> delete_message(std::string by, int64_t message);
    Task<Answer> purge_message(std::string by, int64_t message);
    /* a message with its versions, as the log gives it: "" if there is none */
    Task<std::string> logged_json(DbPtr db, drogon::orm::Result r);
    Task<std::string> offer_json(std::string member);
    /* the entry form, if the Pond asks one of those joining: its JSON ({intro, questions}), or "" */
    Task<std::string> entry_form();
    /* the event and order checks the tick runs: orders waiting on Stripe */
    Task<> check_orders();
    int64_t next_orders_ = 0;
    std::atomic<bool> checking_orders_{false};
    Task<std::string> member_json_of(DbPtr db, std::string id);
    Task<std::string> pond_json(DbPtr db);
    /* the channels `member` may see, in order */
    Task<std::string> channels_json(std::string member);
    Task<std::string> roles_json();
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
