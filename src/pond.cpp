#include "pond.h"

#include "encoding.h"
#include "json.h"
#include "text.h"

#include <drogon/HttpAppFramework.h>
#include <drogon/orm/Exception.h>
#include <drogon/utils/coroutine.h>
#include <trantor/utils/Logger.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <map>

namespace pond {

using drogon::orm::DrogonDbException;
using drogon::orm::Result;

int64_t now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

bool channel_name_ok(const std::string &s)
{
    if (s.empty() || s.size() > 32)
        return false;
    for (char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
            return false;
    return true;
}

std::string new_token()
{
    unsigned char b[16];
    random_bytes(b, sizeof b);
    return base32(b, sizeof b);
}

bool token_ok(const std::string &s)
{
    if (s.size() != 26)
        return false;
    for (char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= '2' && c <= '7')))
            return false;
    return true;
}

Answer fail(int status, const char *code)
{
    return {status, "{\"error\":" + jstr(code) + "}"};
}

static Answer server_error(const DrogonDbException &e)
{
    LOG_ERROR << "database: " << e.base().what();
    return fail(500, "database");
}

static Outcome refused(const DrogonDbException &e)
{
    return std::string("the database refused it: ") + e.base().what();
}

Service::Service(DbPtr db, PondKey key, std::string data_dir, trantor::EventLoop *timers)
    : db_(std::move(db)), key_(std::move(key)), files_dir_(data_dir + "/files"), pictures_dir_(data_dir + "/pictures"), timers_(timers)
{
}

/* ---- JSON for what is kept ---- */

/* "" is stored as NULL. Built here, outside any coroutine, and held in a named variable by the caller (docs/design.md, "Coroutines") */
static std::optional<std::string> nullable(const std::string &s)
{
    if (s.empty())
        return std::nullopt;
    return s;
}

/* each list of roles is from GROUP_CONCAT: ids, with commas, or NULL for none */
static const char MEMBER_COLS[] = "id, profile, joined_ms, role, muted_until, dms,"
                                  " (SELECT GROUP_CONCAT(mr.role ORDER BY mr.role) FROM member_roles mr WHERE mr.member = members.id),"
                                  " role_color";
/* the overrides as "role:allow:deny,..." (docs/design.md, "Permissions") */
static const char CHANNEL_COLS[] = "id, name, topic, position, kind,"
                                   " (SELECT GROUP_CONCAT(CONCAT(o.role, ':', o.allow, ':', o.deny) ORDER BY o.role) FROM channel_overrides o"
                                   " WHERE o.channel = channels.id)";

/* "muted": null, or {"until": ms, or null until they are unmuted} (docs/design.md, "Muting") */
static std::string muted_json(const drogon::orm::Field &f)
{
    if (f.isNull() || (num(f) != 0 && num(f) <= now_ms()))
        return "null";
    return num(f) == 0 ? std::string("{\"until\":null}") : "{\"until\":" + std::to_string(num(f)) + "}";
}

/* GROUP_CONCAT's ids as a JSON array: numbers from the database, so safe as they are */
static std::string id_list(const drogon::orm::Field &f)
{
    return "[" + text(f) + "]";
}

static std::string member_json(const drogon::orm::Row &row) /* MEMBER_COLS */
{
    return "{\"id\":" + jstr(text(row[0])) + ",\"profile\":" + (row[1].isNull() ? std::string("null") : text(row[1])) +
           ",\"joined\":" + std::to_string(num(row[2])) + ",\"role\":" + jstr(text(row[3])) + ",\"muted\":" + muted_json(row[4]) +
           ",\"dms\":" + (num(row[5]) ? "true" : "false") + ",\"roles\":" + id_list(row[6]) +
           ",\"role_color\":" + (num(row[7]) ? "true" : "false") + "}";
}

std::vector<Override> parse_overrides(const std::string &concat)
{
    std::vector<Override> out;
    size_t p = 0;
    while (p < concat.size()) {
        size_t end = concat.find(',', p);
        if (end == std::string::npos)
            end = concat.size();
        Override o;
        if (sscanf(concat.substr(p, end - p).c_str(), "%lld:%lld:%lld", (long long *)&o.role, (long long *)&o.allow, (long long *)&o.deny) == 3)
            out.push_back(o);
        p = end + 1;
    }
    return out;
}

std::string overrides_json(const std::vector<Override> &list)
{
    std::string out = "[";
    for (const Override &o : list)
        out += std::string(out.size() > 1 ? "," : "") + "{\"role\":" + std::to_string(o.role) + ",\"allow\":" + permissions_json(o.allow) +
               ",\"deny\":" + permissions_json(o.deny) + "}";
    return out + "]";
}

static std::string channel_json(const drogon::orm::Row &row) /* CHANNEL_COLS */
{
    const std::vector<Override> list = parse_overrides(text(row[5]));
    /* `roles`, as a Puddle of protocol 9 to 12 reads it: a channel that denies everyone the view is one only the roles allowed
     * it see (null: open to everyone) */
    bool restricted = false;
    std::string roles;
    for (const Override &o : list) {
        if (o.role == 0 && (o.deny & VIEW_CHANNELS))
            restricted = true;
        if (o.role != 0 && (o.allow & VIEW_CHANNELS))
            roles += (roles.empty() ? "" : ",") + std::to_string(o.role);
    }
    return "{\"id\":" + std::to_string(num(row[0])) + ",\"name\":" + jstr(text(row[1])) + ",\"topic\":" + jstr(text(row[2])) +
           ",\"position\":" + std::to_string(num(row[3])) + ",\"kind\":" + jstr(text(row[4])) +
           ",\"roles\":" + (restricted ? "[" + roles + "]" : std::string("null")) + ",\"overrides\":" + overrides_json(list) + "}";
}

/* `extra` is what follows `ts` and `edited`: the message's "files", "reply", "reactions" and "mentions", each with its comma */
static std::string message_json(const drogon::orm::Row &row, const std::string &extra = "") /* MESSAGE_COLS */
{
    /* `edited`: when it was last edited (protocol 14), only on one that was */
    const std::string edited = row[5].isNull() ? "" : ",\"edited\":" + std::to_string(num(row[5]));
    return "{\"id\":" + std::to_string(num(row[0])) + ",\"channel\":" + std::to_string(num(row[1])) + ",\"author\":" + jstr(text(row[2])) +
           ",\"text\":" + jstr(text(row[3])) + ",\"ts\":" + std::to_string(num(row[4])) + edited + extra + "}";
}

/* The first `max` characters of UTF-8 text, with "…" when there were more */
static std::string first_chars(const std::string &s, size_t max)
{
    size_t chars = 0;
    for (size_t i = 0; i < s.size(); i++) {
        if (((unsigned char)s[i] & 0xC0) == 0x80)
            continue;
        if (chars++ == max)
            return s.substr(0, i) + "\xE2\x80\xA6";
    }
    return s;
}

/* Rows of MESSAGE_COLS as a JSON array, each with the files it carries (not those an edit dropped), and (protocol 15) what it
 * replies to, its reactions and its mentions. */
Task<std::string> Service::messages_json(DbPtr db, Result r)
{
    std::map<int64_t, std::string> files, reactions;
    std::map<int64_t, std::string> quotes; /* a message replied to: its `reply` */
    if (!r.empty()) {
        std::string ids, replied;
        for (size_t i = 0; i < r.size(); i++) {
            ids += (i ? "," : "") + std::to_string(num(r[i][0])); /* numbers from the database: safe to put in the statement */
            if (!r[i][6].isNull())
                replied += (replied.empty() ? "" : ",") + std::to_string(num(r[i][6]));
        }
        Result f = co_await db->execSqlCoro("SELECT message, file, name, mime, size, w, h, duration_ms, preview FROM message_files"
                                            " WHERE message IN (" + ids + ") AND removed_ms IS NULL ORDER BY message, position");
        for (const auto &row : f) {
            std::string &out = files[num(row[0])];
            out += out.empty() ? "[" : ",";
            out += "{\"id\":" + jstr(text(row[1])) + ",\"name\":" + jstr(text(row[2])) + ",\"mime\":" + jstr(text(row[3])) +
                   ",\"size\":" + std::to_string(num(row[4]));
            if (num(row[5]) > 0 && num(row[6]) > 0)
                out += ",\"w\":" + std::to_string(num(row[5])) + ",\"h\":" + std::to_string(num(row[6]));
            if (num(row[7]) > 0)
                out += ",\"duration_ms\":" + std::to_string(num(row[7]));
            if (!text(row[8]).empty())
                out += ",\"preview\":" + jstr(text(row[8]));
            out += "}";
        }
        for (auto &[id, a] : files)
            a += "]";
        /* reactions: each emoji in the order it was first used, with who used it, in order */
        Result re = co_await db->execSqlCoro("SELECT message, emoji, member FROM reactions WHERE message IN (" + ids + ") ORDER BY message, ts_ms, member");
        std::map<int64_t, std::vector<std::pair<std::string, std::vector<std::string>>>> by;
        for (const auto &row : re) {
            auto &list = by[num(row[0])];
            const std::string e = text(row[1]);
            auto it = std::find_if(list.begin(), list.end(), [&](const auto &p) { return p.first == e; });
            if (it == list.end())
                it = list.insert(list.end(), {e, {}});
            it->second.push_back(text(row[2]));
        }
        for (const auto &[id, list] : by) {
            std::string out = "[";
            for (const auto &[e, who] : list) {
                std::string members;
                for (const std::string &m : who)
                    members += (members.empty() ? "" : ",") + jstr(m);
                out += std::string(out.size() > 1 ? "," : "") + "{\"emoji\":" + jstr(e) + ",\"count\":" + std::to_string(who.size()) +
                       ",\"members\":[" + members + "]}";
            }
            reactions[id] = out + "]";
        }
        /* what each reply quotes: the message as it reads now, or that it is gone */
        if (!replied.empty()) {
            Result q = co_await db->execSqlCoro("SELECT m.id, m.author, m.text, m.deleted_ms, (SELECT COUNT(*) FROM message_files f WHERE"
                                                " f.message = m.id AND f.removed_ms IS NULL) FROM messages m WHERE m.id IN (" + replied + ")");
            for (const auto &row : q) {
                if (!row[3].isNull())
                    continue;
                std::string one = "{\"id\":" + std::to_string(num(row[0])) + ",\"author\":" + jstr(text(row[1])) +
                                  ",\"text\":" + jstr(first_chars(text(row[2]), QUOTE_MAX));
                if (num(row[4]) > 0)
                    one += ",\"files\":" + std::to_string(num(row[4]));
                quotes[num(row[0])] = one + "}";
            }
        }
    }
    std::string out = "[";
    for (size_t i = 0; i < r.size(); i++) {
        const int64_t id = num(r[i][0]);
        std::string extra;
        if (auto it = files.find(id); it != files.end())
            extra += ",\"files\":" + it->second;
        if (!r[i][6].isNull()) {
            auto q = quotes.find(num(r[i][6]));
            extra += ",\"reply\":" + (q != quotes.end() ? q->second : "{\"id\":" + std::to_string(num(r[i][6])) + ",\"deleted\":true}");
        }
        if (auto it = reactions.find(id); it != reactions.end())
            extra += ",\"reactions\":" + it->second;
        if (!r[i][7].isNull() && !text(r[i][7]).empty())
            extra += ",\"mentions\":" + text(r[i][7]);
        out += (i ? "," : "") + message_json(r[i], extra);
    }
    co_return out + "]";
}

Task<std::string> Service::member_json_of(DbPtr db, std::string id)
{
    Result r = co_await db->execSqlCoro(std::string("SELECT ") + MEMBER_COLS + " FROM members WHERE id = ?", id);
    if (r.empty())
        co_return "";
    co_return member_json(r[0]);
}

Task<std::string> Service::pond_json(DbPtr db)
{
    Result r = co_await db->execSqlCoro("SELECT name, value FROM settings WHERE name IN ('name', 'description', 'files.max_size', 'icon', 'banner', 'dms', 'dms.files_max', 'role_colors', 'everyone', 'stripe_key')");
    std::map<std::string, std::string> v;
    for (const auto &row : r)
        v[text(row[0])] = text(row[1]);
    const std::string max = v.count("files.max_size") ? v["files.max_size"] : std::to_string(FILES_DEFAULT_MAX);
    const std::string dm_max = v.count("dms.files_max") ? v["dms.files_max"] : std::to_string(DM_FILES_DEFAULT_MAX);
    /* the icon (docs/design.md, "Pictures"): {hash, mime, size}, or null when there is none (or its file went) */
    std::string icon = "null";
    if (!v["icon"].empty() && !picture_ref(v["icon"], icon))
        icon = "null";
    /* the banner (protocol 11, docs/design.md, "Pages"): over its name in the channel list; the same, or null */
    std::string banner = "null";
    if (!v["banner"].empty() && !picture_ref(v["banner"], banner))
        banner = "null";
    /* direct messages (docs/dms.md): on or off, and the largest file one may carry (0: none) */
    const std::string dms = std::string("{\"on\":") + (v["dms"] != "off" ? "true" : "false") + ",\"files_max\":" +
                            std::to_string(strtoll(dm_max.c_str(), nullptr, 10)) + "}";
    /* what members may send: "files" (docs/design.md, "Files"); max_size 0 is files turned off */
    co_return "{\"id\":" + jstr(key_.id) + ",\"key\":" + jstr(hex(key_.pub, 32)) + ",\"seal_key\":" + jstr(hex(key_.seal_pub, 32)) + ",\"name\":" + jstr(v["name"]) + ",\"description\":" + jstr(v["description"]) +
              ",\"icon\":" + icon + ",\"banner\":" + banner + ",\"files\":{\"max_size\":" + std::to_string(strtoll(max.c_str(), nullptr, 10)) + ",\"chunk_size\":" +
              std::to_string(FILE_CHUNK) + ",\"per_message\":" + std::to_string(FILES_PER_MESSAGE) + "},\"dms\":" + dms +
              /* role colours (protocol 12): "choice", each member's own, or "enforced", everyone's name in their role's colour */
              ",\"role_colors\":" + jstr(v["role_colors"] == "enforced" ? "enforced" : "choice") +
              /* what everyone may do (protocol 13, "Permissions") */
              ",\"everyone\":{\"permissions\":" + permissions_json(v["everyone"].empty() ? EVERYONE_DEFAULT : strtoll(v["everyone"].c_str(), nullptr, 10) & ALL_PERMISSIONS) + "}" +
              /* whether the shop takes payments, and whether they are only tests (protocol 13, "Shop"); the key itself never goes */
              ",\"shop\":{\"payments\":" + (v["stripe_key"].empty() ? "false" : "true") +
              ",\"test\":" + (v["stripe_key"].rfind("sk_live_", 0) == 0 || v["stripe_key"].rfind("rk_live_", 0) == 0 ? "false" : "true") + "}}";
}

Task<std::string> Service::channels_json(std::string member)
{
    const std::set<int64_t> hidden = co_await hidden_channels(member);
    Result r = co_await db_->execSqlCoro(std::string("SELECT ") + CHANNEL_COLS + " FROM channels WHERE deleted = 0 ORDER BY position, id");
    std::string out = "[";
    for (size_t i = 0; i < r.size(); i++)
        if (!hidden.count(num(r[i][0])))
            out += (out.size() > 1 ? "," : "") + channel_json(r[i]);
    co_return out + "]";
}

Task<std::string> Service::roles_json()
{
    Result r = co_await db_->execSqlCoro("SELECT id, name, color, hoist, permissions FROM roles ORDER BY position, id");
    std::string out = "[";
    for (size_t i = 0; i < r.size(); i++)
        out += std::string(i ? "," : "") + role_json(num(r[i][0]), text(r[i][1]), text(r[i][2]), num(r[i][3]) != 0, num(r[i][4]));
    co_return out + "]";
}


Task<std::string> Service::setting(std::string name)
{
    Result r = co_await db_->execSqlCoro("SELECT value FROM settings WHERE name = ?", name);
    if (r.empty())
        co_return "";
    co_return text(r[0][0]);
}

/* ---- members ---- */

Task<std::optional<std::array<unsigned char, 32>>> Service::member_key(std::string id, std::string kid)
{
    try {
        Result r = co_await db_->execSqlCoro("SELECT sign_pub FROM members WHERE id = ? AND kid = ? AND left_ms IS NULL", id, kid);
        std::array<unsigned char, 32> pub;
        if (!r.empty() && unhex(text(r[0][0]), pub.data(), 32))
            co_return pub;
    } catch (const DrogonDbException &e) {
        LOG_ERROR << "database: " << e.base().what();
    }
    co_return std::nullopt;
}

Task<bool> Service::is_member(std::string id)
{
    Result r = co_await db_->execSqlCoro("SELECT 1 FROM members WHERE id = ? AND left_ms IS NULL", id);
    co_return !r.empty();
}

Task<Answer> Service::join(Chain chain, std::string invite, std::string profile)
{
    Answer a = co_await enter(chain, invite, profile);
    if (a.status != 200)
        co_return a; /* if, not ?: (docs/design.md, "Coroutines") */
    co_return co_await info(chain.id);
}

/* Joins at the same moment with the same invite wait on its row (enter_once). Should MariaDB still stop one ("record has
 * changed since last read", 1020, or a deadlock, 1213), it is tried again a little later, seeing the uses already spent. */
Task<Answer> Service::enter(Chain chain, std::string invite, std::string profile)
{
    for (int attempt = 1;; attempt++) {
        Answer a = co_await enter_once(chain, invite, profile);
        if (a.status != 0)
            co_return a;
        if (attempt == JOIN_TRIES) {
            LOG_ERROR << "database: a join met others " << JOIN_TRIES << " times";
            co_return fail(500, "database");
        }
        co_await drogon::sleepCoro(drogon::app().getLoop(), 0.005 * attempt * (1 + std::rand() % 4)); /* apart, not all at once again */
    }
}

Task<Answer> Service::enter_once(Chain chain, std::string invite, std::string profile)
{
    TxPtr tx;
    try {
        tx = co_await db_->newTransactionCoro();
        /* the invite's row first, locked: joins with the same invite go one at a time, and this is read as it is now, before
         * anything else in the transaction fixes what it sees. The member's row is only read: locking one that isn't there
         * yet would lock the gap it goes in, and two joins inserting into the same gap stop each other (a deadlock). */
        co_await tx->execSqlCoro("SELECT 1 FROM invites WHERE token = ? FOR UPDATE", invite);
        Result r = co_await tx->execSqlCoro("SELECT left_ms FROM members WHERE id = ?", chain.id);
        bool member = !r.empty() && r[0][0].isNull();
        int64_t now = now_ms();
        Result banned = co_await tx->execSqlCoro("SELECT 1 FROM bans WHERE member = ?", chain.id);
        if (!banned.empty()) { /* answered as an invite that does not work: a ban says nothing either */
            tx->rollback();
            co_return fail(404, "not_found");
        }
        if (!member) {
            /* a Pond with an entry form: those not accepted yet apply first (applications.cpp) */
            const std::string form = co_await entry_form();
            if (!form.empty()) {
                Result a = co_await tx->execSqlCoro("SELECT status, reason FROM applications WHERE member = ?", chain.id);
                const std::string status = a.empty() ? "" : text(a[0][0]);
                if (status != "accepted") {
                    Result ok = co_await tx->execSqlCoro("SELECT 1 FROM invites WHERE token = ? AND revoked = 0 AND (max_uses IS NULL OR uses < max_uses)"
                                                         " AND (expires_ms IS NULL OR expires_ms > ?)",
                                                         invite, now);
                    tx->rollback();
                    if (status == "pending")
                        co_return fail(409, "application_pending");
                    if (status == "declined")
                        co_return Answer{409, "{\"error\":\"application_declined\",\"reason\":" + jstr(text(a[0][1])) + ",\"form\":" + form + "}"};
                    if (ok.empty())
                        co_return fail(404, "not_found");
                    co_return Answer{409, "{\"error\":\"application_required\",\"form\":" + form + "}"};
                }
            }
            /* spend one use, in the same statement that checks the invite may still be used, so two joins can't share the last */
            Result u = co_await tx->execSqlCoro("UPDATE invites SET uses = uses + 1 WHERE token = ? AND revoked = 0"
                                                " AND (max_uses IS NULL OR uses < max_uses) AND (expires_ms IS NULL OR expires_ms > ?)",
                                                invite, now);
            if (u.affectedRows() != 1) {
                tx->rollback();
                co_return fail(404, "not_found");
            }
        }
        const std::optional<std::string> shown = nullable(profile);
        co_await tx->execSqlCoro("INSERT INTO members (id, sign_pub, kid, seq, chain, profile, joined_ms) VALUES (?, ?, ?, ?, ?, ?, ?)"
                                 " ON DUPLICATE KEY UPDATE sign_pub = VALUES(sign_pub), kid = VALUES(kid), seq = VALUES(seq),"
                                 " chain = VALUES(chain), profile = VALUES(profile),"
                                 " joined_ms = IF(left_ms IS NULL, joined_ms, VALUES(joined_ms)), left_ms = NULL",
                                 chain.id, chain.sign_pub_hex, chain.kid, (int64_t)chain.seq, chain.text, shown, now);
        std::string me = co_await member_json_of(tx, chain.id);
        int64_t ev = co_await add_event(tx, "member", me);
        if (!co_await Committed(std::move(tx)))
            co_return fail(500, "database");
        bump(ev);
        if (!member)
            LOG_INFO << chain.id << " joined with invite " << invite.substr(0, 6) << "...";
        co_return Answer{200, ""};
    } catch (const DrogonDbException &e) {
        if (tx)
            tx->rollback();
        const int why = sql_errno(e);
        if (why == 1020 || why == 1213)
            co_return Answer{0, ""};
        co_return server_error(e);
    }
}

Task<Answer> Service::keys(std::string member, Chain chain)
{
    if (chain.id != member)
        co_return fail(400, "wrong_chain");
    try {
        co_await db_->execSqlCoro("UPDATE members SET sign_pub = ?, kid = ?, seq = ?, chain = ? WHERE id = ? AND left_ms IS NULL AND seq <= ?",
                                  chain.sign_pub_hex, chain.kid, (int64_t)chain.seq, chain.text, member, (int64_t)chain.seq);
        co_return Answer{204, ""}; /* an older chain than the one held changes nothing */
    } catch (const DrogonDbException &e) {
        co_return server_error(e);
    }
}

Task<Answer> Service::info(std::string member)
{
    try {
        std::string me = co_await member_json_of(db_, member);
        Result c = co_await db_->execSqlCoro("SELECT COALESCE(MAX(id), 0) FROM events");
        if (me.empty())
            co_return fail(500, "database");
        std::string features = "[";
        for (const char *f : FEATURES)
            features += (features.size() > 1 ? "," : "") + jstr(f);
        if (voice_now)
            features += ",\"voice\"";
        const DmSettings dms = co_await dm_settings();
        if (dms.on)
            features += dms.files_max > 0 ? ",\"dms\",\"dm_files\"" : ",\"dms\"";
        /* each co_await on its own, then the text (docs/design.md, "Coroutines") */
        const std::string pond = co_await pond_json(db_);
        const std::string offer = co_await offer_json(member);
        const std::string channels = co_await channels_json(member);
        const std::string roles = co_await roles_json();
        const std::string pages = co_await pages_json_for(member);
        const std::set<int64_t> hidden = co_await hidden_channels(member);
        const std::string voice = voice_now ? voice_now(hidden) : "[]";
        std::string body = "{\"protocol\":" + std::to_string(PROTOCOL) + ",\"features\":" + features + "],\"pond\":" + pond +
                           ",\"me\":" + me + ",\"offer\":" + offer + ",\"channels\":" + channels + ",\"roles\":" + roles + ",\"pages\":" + pages + ",\"voice\":" + voice +
                           ",\"cursor\":" + std::to_string(num(c[0][0])) + "}";
        co_return Answer{200, std::move(body)};
    } catch (const DrogonDbException &e) {
        co_return server_error(e);
    }
}

Task<Answer> Service::history(std::string member, int64_t channel, int64_t before, int limit)
{
    try {
        Result live = co_await db_->execSqlCoro("SELECT 1 FROM channels WHERE id = ? AND deleted = 0", channel);
        const bool seen = co_await may_see(member, channel);
        if (live.empty() || !seen)
            co_return fail(404, "no_channel");
        Result r = co_await db_->execSqlCoro(std::string("SELECT ") + MESSAGE_COLS +
                                                 " FROM messages WHERE channel = ? AND id < ? AND deleted_ms IS NULL ORDER BY id DESC LIMIT ?",
                                             channel, before > 0 ? before : INT64_MAX, (int64_t)limit);
        const std::string messages = co_await messages_json(db_, r);
        co_return Answer{200, "{\"messages\":" + messages + "}"};
    } catch (const DrogonDbException &e) {
        co_return server_error(e);
    }
}

/* What a message says of a file, cleaned: false if it cannot be. */
bool clean_file(FileRef &f)
{
    std::string name;
    if (!clean_text(f.name, FILE_NAME_MAX, false, name) || name.empty() || name.size() > FILE_NAME_MAX || name == "." || name == "..")
        return false;
    for (char c : name)
        if (c == '/' || c == '\\')
            return false;
    f.name = name;
    bool slash = false;
    for (char &c : f.mime) {
        c = (char)tolower((unsigned char)c);
        slash = slash || c == '/';
        if (!isalnum((unsigned char)c) && !strchr("/!#$&^_.+-", c))
            return false;
    }
    if (f.mime.size() > FILE_MIME_MAX || !slash)
        f.mime = "application/octet-stream";
    if (f.preview.size() > FILE_PREVIEW_MAX || f.w < 0 || f.h < 0 || f.duration_ms < 0 || f.w > 100000 || f.h > 100000)
        return false;
    for (char c : f.preview)
        if (!isalnum((unsigned char)c) && c != '+' && c != '/' && c != '=')
            return false;
    if (!f.w || !f.h)
        f.w = f.h = 0;
    return true;
}

Task<Answer> Service::send(std::string member, int64_t channel, std::string in, std::string nonce, std::vector<FileRef> files,
                           int64_t reply_to)
{
    std::string body;
    if (!clean_text(in, TEXT_MAX, true, body) || (body.empty() && files.empty()))
        co_return fail(400, "bad_text");
    if (files.size() > FILES_PER_MESSAGE)
        co_return fail(400, "too_many_files");
    for (auto &f : files)
        if (!clean_file(f))
            co_return fail(400, "bad_file");
    if (!send_limit_.take(member, now_ms()))
        co_return fail(429, "slow_down");
    if (int64_t until = muted_until(member); until >= 0) /* muted: nothing they write is posted (docs/design.md, "Muting") */
        co_return Answer{409, "{\"error\":\"muted\",\"until\":" + (until ? std::to_string(until) : std::string("null")) + "}"};
    TxPtr tx;
    try {
        Result live = co_await db_->execSqlCoro("SELECT 1 FROM channels WHERE id = ? AND deleted = 0", channel);
        const int64_t may = co_await permissions_in(member, channel);
        if (live.empty() || !(may & VIEW_CHANNELS))
            co_return fail(404, "no_channel");
        /* what they may do there (docs/design.md, "Permissions") */
        if (!(may & SEND_MESSAGES))
            co_return fail(409, "no_permission");
        if (!files.empty() && !(may & ATTACH_FILES))
            co_return fail(409, "no_files_here");
        if (!files.empty() && co_await files_max() == 0)
            co_return fail(409, "files_off");
        tx = co_await db_->newTransactionCoro();
        std::vector<int64_t> sizes;
        for (auto &f : files) {
            Result w = co_await tx->execSqlCoro("SELECT size FROM files WHERE id = ? AND whole = 1 LOCK IN SHARE MODE", f.id);
            if (w.empty()) {
                tx->rollback();
                co_return Answer{409, "{\"error\":\"no_file\",\"file\":" + jstr(f.id) + "}"}; /* not here whole: send it first */
            }
            sizes.push_back(num(w[0][0]));
        }
        /* a reply answers a message in the same channel that is still there (protocol 15) */
        std::optional<int64_t> answers;
        if (reply_to > 0) {
            Result q = co_await tx->execSqlCoro("SELECT 1 FROM messages WHERE id = ? AND channel = ? AND deleted_ms IS NULL", reply_to, channel);
            if (q.empty()) {
                tx->rollback();
                co_return fail(400, "bad_reply");
            }
            answers = reply_to;
        }
        const std::string said = co_await mentions_json(tx, member, channel, body);
        const std::optional<std::string> mentions = nullable(said);
        const std::optional<std::string> nonce_or_null = nullable(nonce);
        Result ins = co_await tx->execSqlCoro("INSERT INTO messages (channel, author, text, nonce, ts_ms, reply_to, mentions) VALUES (?, ?, ?, ?, ?, ?, ?)",
                                              channel, member, body, nonce_or_null, now_ms(), answers, mentions);
        int64_t mid = (int64_t)ins.insertId();
        for (size_t i = 0; i < files.size(); i++) {
            const FileRef &f = files[i];
            auto opt = [](int64_t v) { return v > 0 ? std::optional<int64_t>(v) : std::nullopt; };
            const std::optional<std::string> preview = nullable(f.preview);
            co_await tx->execSqlCoro("INSERT INTO message_files (message, position, file, name, mime, size, w, h, duration_ms, preview)"
                                     " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                                     mid, (int64_t)i, f.id, f.name, f.mime, sizes[i], opt(f.w), opt(f.h), opt(f.duration_ms),
                                     preview);
        }
        Result r = co_await tx->execSqlCoro(std::string("SELECT ") + MESSAGE_COLS + " FROM messages WHERE id = ?", mid);
        std::string list = co_await messages_json(tx, r);
        std::string msg = list.substr(1, list.size() - 2);
        if (msg.empty())
            throw std::out_of_range("message");
        int64_t ev = co_await add_event(tx, "message", msg, "", channel, mid);
        if (!co_await Committed(std::move(tx)))
            co_return fail(500, "database");
        bump(ev);
        co_return Answer{200, msg};
    } catch (const DrogonDbException &e) {
        if (tx)
            tx->rollback();
        if (sql_errno(e) != 1062)
            co_return server_error(e);
    } catch (const std::out_of_range &) {
        if (tx)
            tx->rollback();
        co_return fail(500, "database");
    }
    /* sent before (the same nonce), and the answer was lost: answer with what was posted then */
    try {
        Result r = co_await db_->execSqlCoro(std::string("SELECT ") + MESSAGE_COLS + " FROM messages WHERE author = ? AND nonce = ?", member,
                                             nonce);
        std::string list = co_await messages_json(db_, r);
        if (r.empty())
            co_return fail(500, "database");
        co_return Answer{200, list.substr(1, list.size() - 2)};
    } catch (const DrogonDbException &e) {
        co_return server_error(e);
    }
}

Task<Answer> Service::members()
{
    try {
        Result r = co_await db_->execSqlCoro(std::string("SELECT ") + MEMBER_COLS + " FROM members WHERE left_ms IS NULL ORDER BY joined_ms");
        std::string out = "{\"members\":[";
        for (size_t i = 0; i < r.size(); i++)
            out += (i ? "," : "") + member_json(r[i]);
        co_return Answer{200, out + "]}"};
    } catch (const DrogonDbException &e) {
        co_return server_error(e);
    }
}

Task<Answer> Service::set_profile(std::string member, std::string profile)
{
    TxPtr tx;
    try {
        tx = co_await db_->newTransactionCoro();
        const std::optional<std::string> shown = nullable(profile);
        co_await tx->execSqlCoro("UPDATE members SET profile = ? WHERE id = ? AND left_ms IS NULL", shown, member);
        std::string mj = co_await member_json_of(tx, member);
        int64_t ev = co_await add_event(tx, "member", mj);
        if (!co_await Committed(std::move(tx)))
            co_return fail(500, "database");
        bump(ev);
        co_return Answer{204, ""};
    } catch (const DrogonDbException &e) {
        if (tx)
            tx->rollback();
        co_return server_error(e);
    }
}

/* "" if `id` was a member and is not now, or was not one; else why it failed. */
Task<Outcome> Service::remove_member(std::string id)
{
    TxPtr tx;
    try {
        tx = co_await db_->newTransactionCoro();
        /* a role does not outlast membership: someone who comes back comes back a member */
        Result r = co_await tx->execSqlCoro("UPDATE members SET left_ms = ?, role = 'member' WHERE id = ? AND left_ms IS NULL", now_ms(), id);
        co_await tx->execSqlCoro("DELETE FROM role_offers WHERE member = ?", id);
        co_await tx->execSqlCoro("DELETE FROM member_roles WHERE member = ?", id);
        int64_t ev = 0;
        if (r.affectedRows()) {
            const std::string left = "{\"id\":" + jstr(id) + "}";
            ev = co_await add_event(tx, "member_left", left);
        }
        if (!co_await Committed(std::move(tx)))
            co_return "the database did not commit it";
        bump(ev);
        forget_dms(id); /* nothing more is relayed to or from them */
        co_return "";
    } catch (const DrogonDbException &e) {
        if (tx)
            tx->rollback();
        co_return refused(e);
    }
}

Task<Answer> Service::leave(std::string member)
{
    Outcome o = co_await remove_member(member);
    if (!o.empty()) {
        LOG_ERROR << o;
        co_return fail(500, "database");
    }
    LOG_INFO << member << " left";
    co_return Answer{204, ""};
}

/* ---- voice channels ---- */

Task<> Service::voice_event(std::string kind, std::string data)
{
    /* both kinds begin {"channel":N,...}: only those who may see it hear of it */
    int64_t channel = 0;
    if (data.rfind("{\"channel\":", 0) == 0)
        channel = strtoll(data.c_str() + 11, nullptr, 10);
    TxPtr tx;
    try {
        tx = co_await db_->newTransactionCoro();
        int64_t ev = co_await add_event(tx, kind.c_str(), data, "", channel);
        if (co_await Committed(std::move(tx)))
            bump(ev);
    } catch (const DrogonDbException &e) {
        if (tx)
            tx->rollback();
        LOG_ERROR << "database: " << e.base().what();
    }
}

Task<std::vector<std::pair<std::string, int64_t>>> Service::voice_gone(std::vector<std::pair<std::string, int64_t>> in)
{
    const std::vector<int64_t> rights = co_await voice_rights(in);
    std::vector<std::pair<std::string, int64_t>> gone;
    for (size_t i = 0; i < in.size(); i++)
        if (!rights[i])
            gone.push_back(in[i]);
    co_return gone;
}

Task<std::vector<int64_t>> Service::voice_rights(std::vector<std::pair<std::string, int64_t>> in)
{
    std::vector<int64_t> out;
    for (auto &[m, c] : in) {
        const bool member = co_await is_member(m);
        int64_t may = 0;
        if (member) {
            Result r = co_await db_->execSqlCoro("SELECT 1 FROM channels WHERE id = ? AND deleted = 0 AND kind = 'voice'", c);
            if (!r.empty())
                may = co_await permissions_in(m, c);
        }
        out.push_back((may & VIEW_CHANNELS) && (may & CONNECT) ? may : 0);
    }
    co_return out;
}

Task<bool> Service::is_voice_channel(int64_t channel, std::string member)
{
    Result r = co_await db_->execSqlCoro("SELECT 1 FROM channels WHERE id = ? AND deleted = 0 AND kind = 'voice'", channel);
    if (r.empty())
        co_return false;
    const int64_t may = co_await permissions_in(member, channel);
    co_return (may & VIEW_CHANNELS) && (may & CONNECT);
}

/* Moderating voice (docs/voice.md, "Moderation"): with the permission in that channel, and only on someone below them */
Task<std::string> Service::may_moderate(std::string by, std::string target, int64_t channel, int64_t need)
{
    const int64_t may = co_await permissions_in(by, channel);
    if (!(may & need) || !(may & VIEW_CHANNELS))
        co_return "no_permission";
    const std::string why = co_await may_act(by, target);
    co_return why;
}

/* ---- the event log ---- */

Task<int64_t> Service::add_event(TxPtr tx, const char *kind, std::string data, std::string only_for, int64_t channel, int64_t message)
{
    co_await tx->execSqlCoro("SELECT value FROM settings WHERE name = 'event_lock' FOR UPDATE");
    const std::string k(kind);
    const std::optional<std::string> to = nullable(only_for);
    const std::optional<int64_t> about = channel > 0 ? std::optional<int64_t>(channel) : std::nullopt;
    /* `message`: the message it is about, so an edit or a deletion can rewrite what was said of it before */
    const std::optional<int64_t> which = message > 0 ? std::optional<int64_t>(message) : std::nullopt;
    Result r = co_await tx->execSqlCoro("INSERT INTO events (kind, data, ts_ms, only_for, channel, message) VALUES (?, ?, ?, ?, ?, ?)", k,
                                        data, now_ms(), to, about, which);
    co_return (int64_t)r.insertId();
}

struct Service::News {
    Service *s;
    int64_t after;
    double seconds;
    std::string member;

    bool await_ready()
    {
        std::lock_guard<std::mutex> g(s->mu_);
        return s->latest_ > after || s->stopping_ || s->has_inbox_locked(member);
    }
    bool await_suspend(std::coroutine_handle<> h)
    {
        auto w = std::make_shared<Waiter>();
        w->after = after;
        w->member = member;
        w->h = h;
        {
            std::lock_guard<std::mutex> g(s->mu_);
            if (s->latest_ > after || s->stopping_ || s->has_inbox_locked(member))
                return false; /* news came in between: go on */
            s->waiters_.push_back(w);
        }
        s->timers_->runAfter(seconds, [w] { w->wake(); });
        return true;
    }
    void await_resume() {}
};

void Service::bump(int64_t id)
{
    std::vector<std::shared_ptr<Waiter>> wake;
    {
        std::lock_guard<std::mutex> g(mu_);
        if (id <= latest_)
            return;
        latest_ = id;
        std::erase_if(waiters_, [&](const std::shared_ptr<Waiter> &w) {
            if (w->done)
                return true;
            if (w->after >= latest_)
                return false;
            wake.push_back(w);
            return true;
        });
    }
    for (auto &w : wake)
        w->wake();
}

/* Says while a member's Puddle holds a request open on the events: it is following, so direct messages can be relayed to it. */
struct Service::Following {
    Service *s;
    std::string member;
    Following(Service *svc, std::string m) : s(svc), member(std::move(m)) { s->following(member, true); }
    ~Following() { s->following(member, false); }
};

Task<Answer> Service::events(int64_t after, std::string member)
{
    Following here(this, member);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
    for (;;) {
        try {
            Result r = co_await db_->execSqlCoro("SELECT id, kind, data, only_for, channel FROM events WHERE id > ? ORDER BY id LIMIT ?",
                                                 after, (int64_t)EVENTS_MAX);
            int64_t oldest;
            {
                std::lock_guard<std::mutex> g(mu_);
                oldest = oldest_;
            }
            int64_t latest;
            {
                std::lock_guard<std::mutex> g(mu_);
                latest = latest_;
            }
            /* the cursor is from before the oldest event kept: what came between is gone, so start again from /info. With none
             * left at all, an empty answer below a newer cursor says the same. */
            if ((oldest > 0 && after + 1 < oldest && (r.empty() || num(r[0][0]) == oldest)) || (r.empty() && latest > after))
                co_return Answer{200, "{\"reset\":true}"};
            /* direct messages and what goes with them (dms.cpp): for this member alone, from memory, with no number */
            const std::vector<std::string> mine = take_inbox(member);
            if (!r.empty() || !mine.empty()) {
                /* an event for one member is left out for the rest, and one about a channel for those who may not see it now
                 * (docs/design.md, "Roles for channels"); the cursor still moves past it. `channels` is written for each member:
                 * the channels they see now, in order. */
                std::set<int64_t> hidden;
                std::string mine_now, pages_now;
                bool asked = false, asked_pages = false, asked_powers = false, manages_applications = false, manages_shop = false;
                for (size_t i = 0; i < r.size(); i++) {
                    if (!asked && (!r[i][4].isNull() || text(r[i][1]) == "channels")) {
                        hidden = co_await hidden_channels(member);
                        mine_now = co_await channels_json(member);
                        asked = true;
                    }
                    if (!asked_pages && text(r[i][1]) == "pages") { /* the pages this member sees now (protocol 13) */
                        pages_now = co_await pages_json_for(member);
                        asked_pages = true;
                    }
                    if (!asked_powers && (text(r[i][1]) == "applications" || text(r[i][1]) == "orders")) { /* only for those who see them */
                        const Powers pw = co_await powers(member);
                        manages_applications = pw.has(MANAGE_APPLICATIONS);
                        manages_shop = pw.has(MANAGE_SHOP);
                        asked_powers = true;
                    }
                }
                std::string out = "{\"events\":[";
                bool first = true;
                for (size_t i = 0; i < r.size(); i++) {
                    if (!r[i][3].isNull() && text(r[i][3]) != member)
                        continue;
                    if (!r[i][4].isNull() && hidden.count(num(r[i][4])))
                        continue;
                    const std::string kind = text(r[i][1]);
                    if ((kind == "applications" && !manages_applications) || (kind == "orders" && !manages_shop))
                        continue;
                    std::string data = text(r[i][2]);
                    if (kind == "channels")
                        data = "{\"channels\":" + mine_now + "}";
                    else if (kind == "pages")
                        data = "{\"pages\":" + pages_now + "}";
                    out += std::string(first ? "" : ",") + "{\"id\":" + std::to_string(num(r[i][0])) + ",\"kind\":" + jstr(kind) +
                           ",\"data\":" + data + "}";
                    first = false;
                }
                for (const std::string &e : mine) {
                    out += std::string(first ? "" : ",") + e;
                    first = false;
                }
                const int64_t cursor = r.empty() ? after : num(r[r.size() - 1][0]);
                co_return Answer{200, out + "],\"cursor\":" + std::to_string(cursor) + "}"};
            }
        } catch (const DrogonDbException &e) {
            co_return server_error(e);
        }
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0 || stopping_ || !timers_)
            break;
        co_await News{this, after, (double)left / 1000.0, member};
        if (stopping_)
            break;
    }
    co_return Answer{200, "{\"events\":[],\"cursor\":" + std::to_string(after) + "}"};
}

Task<> Service::tick()
{
    try {
        if (now_ms() >= next_prune_) {
            next_prune_ = now_ms() + 3600 * 1000;
            co_await db_->execSqlCoro("DELETE FROM events WHERE ts_ms < ?", now_ms() - EVENTS_KEEP_MS);
            co_await sweep_files();
            co_await sweep_pictures();
        }
        if (now_ms() >= next_orders_ && !checking_orders_.exchange(true)) { /* orders waiting on Stripe (shop.cpp), apart from this */
            next_orders_ = now_ms() + 5000;
            drogon::async_run([this]() -> Task<> {
                co_await check_orders();
                checking_orders_ = false;
            });
        }
        if (now_ms() >= next_mutes_) { /* who is muted, from the app or pondctl: what sends and voice look at */
            next_mutes_ = now_ms() + 2000;
            Result m = co_await db_->execSqlCoro("SELECT id, muted_until FROM members WHERE muted_until IS NOT NULL AND left_ms IS NULL");
            std::map<std::string, int64_t> now;
            for (const auto &row : m)
                now[text(row[0])] = num(row[1]);
            std::lock_guard<std::mutex> g(mu_);
            muted_ = std::move(now);
        }
        Result r = co_await db_->execSqlCoro("SELECT COALESCE(MAX(id), 0), COALESCE(MIN(id), 0) FROM events");
        {
            std::lock_guard<std::mutex> g(mu_);
            oldest_ = num(r[0][1]);
            std::erase_if(waiters_, [](const std::shared_ptr<Waiter> &w) { return w->done.load(); });
        }
        bump(num(r[0][0]));
    } catch (const DrogonDbException &e) {
        LOG_ERROR << "database: " << e.base().what();
    }
    ticking_ = false;
}

void Service::watch()
{
    timers_->runEvery(0.5, [this] {
        if (stopping_ || ticking_.exchange(true))
            return; /* one at a time: a slow database should not pile them up */
        drogon::async_run([this]() -> Task<> { co_await tick(); });
    });
}

void Service::stop()
{
    std::vector<std::shared_ptr<Waiter>> wake;
    {
        std::lock_guard<std::mutex> g(mu_);
        stopping_ = true;
        wake.swap(waiters_);
    }
    for (auto &w : wake)
        w->wake();
}

/* ---- the admin's ---- */

Task<Outcome> Service::set_name(std::string name, std::optional<std::string> description)
{
    std::string n, d;
    if (!clean_text(name, POND_NAME_MAX, false, n) || n.empty())
        co_return "a name is 1 to " + std::to_string(POND_NAME_MAX) + " characters";
    if (description && !clean_text(*description, DESCRIPTION_MAX, true, d))
        co_return "a description is at most " + std::to_string(DESCRIPTION_MAX) + " characters";
    TxPtr tx;
    try {
        tx = co_await db_->newTransactionCoro();
        co_await tx->execSqlCoro("REPLACE INTO settings (name, value) VALUES ('name', ?)", n);
        if (description)
            co_await tx->execSqlCoro("REPLACE INTO settings (name, value) VALUES ('description', ?)", d);
        std::string pj = co_await pond_json(tx);
        int64_t ev = co_await add_event(tx, "pond", pj);
        if (!co_await Committed(std::move(tx)))
            co_return "the database did not commit it";
        bump(ev);
        co_return "";
    } catch (const DrogonDbException &e) {
        if (tx)
            tx->rollback();
        co_return refused(e);
    }
}

Task<Outcome> Service::channel_create(std::string name, std::string topic, std::string kind)
{
    std::string t;
    if (!channel_name_ok(name))
        co_return "a channel name is 1 to 32 of a-z, 0-9 and -";
    if (kind != "text" && kind != "voice")
        co_return "a channel is text or voice";
    if (kind == "voice" && !voice_on)
        co_return "voice is turned off on this Pond";
    if (!clean_text(topic, TOPIC_MAX, false, t))
        co_return "a topic is at most " + std::to_string(TOPIC_MAX) + " characters";
    TxPtr tx;
    try {
        tx = co_await db_->newTransactionCoro();
        Result r = co_await tx->execSqlCoro("INSERT INTO channels (name, topic, position, created_ms, kind)"
                                            " SELECT ?, ?, COALESCE(MAX(position), -1) + 1, ?, ? FROM channels WHERE deleted = 0",
                                            name, t, now_ms(), kind);
        Result c = co_await tx->execSqlCoro(std::string("SELECT ") + CHANNEL_COLS + " FROM channels WHERE id = ?", (int64_t)r.insertId());
        int64_t ev = co_await add_event(tx, "channel", channel_json(c.at(0)), "", num(c.at(0)[0]));
        if (!co_await Committed(std::move(tx)))
            co_return "the database did not commit it";
        bump(ev);
        co_return "";
    } catch (const DrogonDbException &e) {
        if (tx)
            tx->rollback();
        if (sql_errno(e) == 1062)
            co_return "there is already a channel called " + name;
        co_return refused(e);
    }
}

/* Change one column of a live channel and tell the members. */
Task<Outcome> Service::channel_set(std::string name, const char *column, std::string value)
{
    TxPtr tx;
    try {
        tx = co_await db_->newTransactionCoro();
        Result r = co_await tx->execSqlCoro(std::string("UPDATE channels SET ") + column + " = ? WHERE name = ? AND deleted = 0", value, name);
        if (r.affectedRows() == 0) { /* none by that name, or nothing changed */
            tx->rollback();
            Result e = co_await db_->execSqlCoro("SELECT 1 FROM channels WHERE name = ? AND deleted = 0", name);
            if (e.empty())
                co_return "no channel called " + name;
            co_return "";
        }
        const std::string now_called = std::string(column) == "name" ? value : name;
        Result c = co_await tx->execSqlCoro(std::string("SELECT ") + CHANNEL_COLS + " FROM channels WHERE name = ? AND deleted = 0", now_called);
        int64_t ev = co_await add_event(tx, "channel", channel_json(c.at(0)), "", num(c.at(0)[0]));
        if (!co_await Committed(std::move(tx)))
            co_return "the database did not commit it";
        bump(ev);
        co_return "";
    } catch (const DrogonDbException &e) {
        if (tx)
            tx->rollback();
        if (sql_errno(e) == 1062)
            co_return "there is already a channel called " + value;
        co_return refused(e);
    }
}

Task<Outcome> Service::channel_rename(std::string name, std::string to)
{
    if (!channel_name_ok(to))
        co_return "a channel name is 1 to 32 of a-z, 0-9 and -";
    co_return co_await channel_set(name, "name", to);
}

Task<Outcome> Service::channel_topic(std::string name, std::string topic)
{
    std::string t;
    if (!clean_text(topic, TOPIC_MAX, false, t))
        co_return "a topic is at most " + std::to_string(TOPIC_MAX) + " characters";
    co_return co_await channel_set(name, "topic", t);
}

Task<Outcome> Service::channel_delete(std::string name)
{
    TxPtr tx;
    try {
        tx = co_await db_->newTransactionCoro();
        Result r = co_await tx->execSqlCoro("SELECT id FROM channels WHERE name = ? AND deleted = 0 FOR UPDATE", name);
        if (r.empty()) {
            tx->rollback();
            co_return "no channel called " + name;
        }
        int64_t id = num(r[0][0]);
        co_await tx->execSqlCoro("UPDATE channels SET deleted = 1 WHERE id = ?", id);
        int64_t ev = co_await add_event(tx, "channel_deleted", "{\"id\":" + std::to_string(id) + "}");
        if (!co_await Committed(std::move(tx)))
            co_return "the database did not commit it";
        bump(ev);
        co_return "";
    } catch (const DrogonDbException &e) {
        if (tx)
            tx->rollback();
        co_return refused(e);
    }
}

Task<Outcome> Service::invite_create(int max_uses, int64_t expires_ms, std::shared_ptr<std::string> token)
{
    *token = new_token();
    try {
        const std::optional<int64_t> uses = max_uses > 0 ? std::optional<int64_t>(max_uses) : std::nullopt;
        const std::optional<int64_t> until = expires_ms > 0 ? std::optional<int64_t>(expires_ms) : std::nullopt;
        co_await db_->execSqlCoro("INSERT INTO invites (token, max_uses, expires_ms, created_ms) VALUES (?, ?, ?, ?)", *token, uses, until,
                                  now_ms());
        co_return "";
    } catch (const DrogonDbException &e) {
        co_return refused(e);
    }
}

Task<Outcome> Service::invite_revoke(std::string token)
{
    try {
        Result r = co_await db_->execSqlCoro("UPDATE invites SET revoked = 1 WHERE token = ?", token);
        if (r.affectedRows() == 0) {
            Result e = co_await db_->execSqlCoro("SELECT 1 FROM invites WHERE token = ?", token);
            if (e.empty())
                co_return "no invite " + token;
        }
        co_return "";
    } catch (const DrogonDbException &e) {
        co_return refused(e);
    }
}

Task<Outcome> Service::member_remove(std::string id)
{
    try {
        if (!co_await is_member(id))
            co_return "no member " + id;
    } catch (const DrogonDbException &e) {
        co_return refused(e);
    }
    co_return co_await remove_member(id);
}

} // namespace pond
