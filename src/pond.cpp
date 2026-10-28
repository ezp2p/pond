#include "pond.h"

#include "base/encoding.h"
#include "base/json.h"
#include "base/log.h"
#include "base/text.h"
#include "core/ratelimit.h"
#include "platform/clock.h"

#include <openssl/rand.h>

#include <chrono>
#include <cstring>
#include <functional>

namespace pond {

int64_t now_ms()
{
    return puddle_wall_ms();
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
    char out[32];
    RAND_bytes(b, sizeof b);
    puddle_base32_encode(b, sizeof b, out, sizeof out);
    return out;
}

/* Text reaches here cleaned (valid UTF-8, no control characters but newlines), so only quotes, backslashes and control
 * characters need escaping. */
std::string jstr(const std::string &s)
{
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') {
            o += '\\';
            o += (char)c;
        } else if (c == '\n') {
            o += "\\n";
        } else if (c < 0x20) {
            char u[8];
            snprintf(u, sizeof u, "\\u%04x", c);
            o += u;
        } else {
            o += (char)c;
        }
    }
    return o + "\"";
}

Answer fail(int status, const char *code)
{
    return {status, std::string("{\"error\":") + jstr(code) + "}"};
}

static Answer server_error()
{
    return fail(500, "database");
}

/* Clean `in` as text from a member: false if it is empty after trimming or longer than `max` characters. */
static bool clean(const std::string &in, size_t max, bool multiline, std::string &out)
{
    std::string buf(in.size() * 3 + 4, '\0'); /* bad bytes become three-byte U+FFFD */
    size_t chars = 0;
    if (puddle_clean_text(in.data(), in.size(), max, multiline, buf.data(), buf.size(), &chars) != 0)
        return false;
    out = buf.c_str();
    return true;
}

Service::Service(Pool &pool, PondKey key) : pool_(pool), key_(std::move(key))
{
    send_limit_ = puddle_keyed_new(4096, 20, 4000); /* 5 a second on average, 20 at once */
}

Service::~Service()
{
    puddle_keyed_free(send_limit_);
}

/* ---- members ---- */

bool Service::member_key(const std::string &id, const std::string &kid, unsigned char pub[32])
{
    auto db = pool_.get();
    Result r;
    if (!db || !db->exec("SELECT sign_pub FROM members WHERE id = ? AND kid = ? AND left_ms IS NULL", {id, kid}, &r) || r.rows.empty())
        return false;
    return puddle_hex_decode(text(r.rows[0][0]).c_str(), pub, 32) == 0;
}

bool Service::is_member(const std::string &id)
{
    auto db = pool_.get();
    Result r;
    return db && db->exec("SELECT 1 FROM members WHERE id = ? AND left_ms IS NULL", {id}, &r) && !r.rows.empty();
}

std::string Service::member_json(const std::vector<Cell> &row)
{
    return "{\"id\":" + jstr(text(row[0])) + ",\"profile\":" + (row[1] ? *row[1] : std::string("null")) +
           ",\"joined\":" + std::to_string(num(row[2])) + "}";
}

static const char MEMBER_COLS[] = "id, profile, joined_ms";

std::string Service::setting(const std::string &name)
{
    auto db = pool_.get();
    Result r;
    if (!db || !db->exec("SELECT value FROM settings WHERE name = ?", {name}, &r) || r.rows.empty())
        return "";
    return text(r.rows[0][0]);
}

std::string Service::pond_json(Db &db)
{
    Result r;
    std::string name, description;
    if (db.exec("SELECT name, value FROM settings WHERE name IN ('name', 'description')", {}, &r))
        for (auto &row : r.rows)
            (text(row[0]) == "name" ? name : description) = text(row[1]);
    return "{\"id\":" + jstr(key_.id) + ",\"name\":" + jstr(name) + ",\"description\":" + jstr(description) + "}";
}

static std::string channel_json(const std::vector<Cell> &row) /* id, name, topic, position */
{
    return "{\"id\":" + std::to_string(num(row[0])) + ",\"name\":" + jstr(text(row[1])) + ",\"topic\":" + jstr(text(row[2])) +
           ",\"position\":" + std::to_string(num(row[3])) + "}";
}

std::string Service::channels_json(Db &db)
{
    Result r;
    std::string out = "[";
    if (db.exec("SELECT id, name, topic, position FROM channels WHERE deleted = 0 ORDER BY position, id", {}, &r))
        for (size_t i = 0; i < r.rows.size(); i++)
            out += (i ? "," : "") + channel_json(r.rows[i]);
    return out + "]";
}

Answer Service::join(const Chain &chain, const std::string &invite, const std::string &profile)
{
    Answer a = enter(chain, invite, profile);
    return a.status == 200 ? info(chain.id) : a;
}

/* join(), up to answering: one connection, given back before info() takes one. */
Answer Service::enter(const Chain &chain, const std::string &invite, const std::string &profile)
{
    auto db = pool_.get();
    if (!db || !db->begin())
        return server_error();
    Result r;
    if (!db->exec("SELECT left_ms FROM members WHERE id = ? FOR UPDATE", {chain.id}, &r)) {
        db->rollback();
        return server_error();
    }
    bool member = !r.rows.empty() && !r.rows[0][0];
    int64_t now = now_ms();
    if (!member) {
        /* spend one use, in the same statement that checks the invite may still be used, so two joins can't share the last use */
        Result u;
        if (!db->exec("UPDATE invites SET uses = uses + 1 WHERE token = ? AND revoked = 0 AND (max_uses IS NULL OR uses < max_uses)"
                      " AND (expires_ms IS NULL OR expires_ms > ?)",
                      {invite, now}, &u)) {
            db->rollback();
            return server_error();
        }
        if (u.affected != 1) {
            db->rollback();
            return fail(404, "not_found");
        }
    }
    Value prof = profile.empty() ? Value(nullptr) : Value(profile);
    bool ok = db->exec("INSERT INTO members (id, sign_pub, kid, seq, chain, profile, joined_ms) VALUES (?, ?, ?, ?, ?, ?, ?)"
                       " ON DUPLICATE KEY UPDATE sign_pub = VALUES(sign_pub), kid = VALUES(kid), seq = VALUES(seq),"
                       " chain = VALUES(chain), profile = VALUES(profile),"
                       " joined_ms = IF(left_ms IS NULL, joined_ms, VALUES(joined_ms)), left_ms = NULL",
                       {chain.id, chain.sign_pub_hex, chain.kid, (int64_t)chain.seq, chain.text, prof, now});
    Result m;
    ok = ok && db->exec(std::string("SELECT ") + MEMBER_COLS + " FROM members WHERE id = ?", {chain.id}, &m) && !m.rows.empty();
    std::string me = ok ? member_json(m.rows[0]) : "";
    if (!ok || !add_event(*db, "member", me) || !db->commit()) {
        db->rollback();
        return server_error();
    }
    if (!member)
        puddle_log(PUDDLE_LOG_INFO, "%s joined with invite %.6s...", chain.id.c_str(), invite.c_str());
    return {200, ""};
}

Answer Service::keys(const std::string &member, const Chain &chain)
{
    if (chain.id != member)
        return fail(400, "wrong_chain");
    auto db = pool_.get();
    Result r;
    if (!db || !db->exec("UPDATE members SET sign_pub = ?, kid = ?, seq = ?, chain = ? WHERE id = ? AND left_ms IS NULL AND seq <= ?",
                         {chain.sign_pub_hex, chain.kid, (int64_t)chain.seq, chain.text, member, (int64_t)chain.seq}, &r))
        return server_error();
    return {204, ""}; /* an older chain than the one held changes nothing */
}

Answer Service::info(const std::string &member)
{
    auto db = pool_.get();
    Result m, c;
    if (!db || !db->exec(std::string("SELECT ") + MEMBER_COLS + " FROM members WHERE id = ?", {member}, &m) || m.rows.empty() ||
        !db->exec("SELECT COALESCE(MAX(id), 0) FROM events", {}, &c))
        return server_error();
    return {200, "{\"pond\":" + pond_json(*db) + ",\"me\":" + member_json(m.rows[0]) + ",\"channels\":" + channels_json(*db) +
                     ",\"cursor\":" + std::to_string(num(c.rows[0][0])) + "}"};
}

bool Service::live_channel(Db &db, const std::string &key, int64_t &id, std::string *json, bool by_name)
{
    Result r;
    if (!db.exec(std::string("SELECT id, name, topic, position FROM channels WHERE deleted = 0 AND ") + (by_name ? "name" : "id") + " = ?",
                 {by_name ? Value(key) : Value((int64_t)strtoll(key.c_str(), nullptr, 10))}, &r) ||
        r.rows.empty())
        return false;
    id = num(r.rows[0][0]);
    if (json)
        *json = channel_json(r.rows[0]);
    return true;
}

static std::string message_json(const std::vector<Cell> &row) /* id, channel, author, text, ts_ms */
{
    return "{\"id\":" + std::to_string(num(row[0])) + ",\"channel\":" + std::to_string(num(row[1])) + ",\"author\":" + jstr(text(row[2])) +
           ",\"text\":" + jstr(text(row[3])) + ",\"ts\":" + std::to_string(num(row[4])) + "}";
}

static const char MESSAGE_COLS[] = "id, channel, author, text, ts_ms";

Answer Service::history(const std::string &channel, int64_t before, int limit)
{
    auto db = pool_.get();
    int64_t ch;
    if (!db)
        return server_error();
    if (!live_channel(*db, channel, ch, nullptr, false))
        return fail(404, "no_channel");
    Result r;
    if (!db->exec(std::string("SELECT ") + MESSAGE_COLS + " FROM messages WHERE channel = ? AND id < ? ORDER BY id DESC LIMIT ?",
                  {ch, before > 0 ? before : INT64_MAX, (int64_t)limit}, &r))
        return server_error();
    std::string out = "{\"messages\":[";
    for (size_t i = 0; i < r.rows.size(); i++)
        out += (i ? "," : "") + message_json(r.rows[i]);
    return {200, out + "]}"};
}

Answer Service::send(const std::string &member, const std::string &channel, const std::string &in, const std::string &nonce)
{
    std::string body;
    if (!clean(in, TEXT_MAX, true, body) || body.empty())
        return fail(400, "bad_text");
    if (!puddle_keyed_hit(send_limit_, member.c_str(), now_ms()))
        return fail(429, "slow_down");
    auto db = pool_.get();
    int64_t ch;
    if (!db)
        return server_error();
    if (!live_channel(*db, channel, ch, nullptr, false))
        return fail(404, "no_channel");
    if (!db->begin())
        return server_error();
    Result ins;
    if (!db->exec("INSERT INTO messages (channel, author, text, nonce, ts_ms) VALUES (?, ?, ?, ?, ?)",
                  {ch, member, body, nonce.empty() ? Value(nullptr) : Value(nonce), now_ms()}, &ins)) {
        bool again = db->last_errno() == 1062;
        db->rollback();
        if (!again)
            return server_error();
        /* sent before, and the answer was lost: answer with what was posted then */
        Result r;
        if (!db->exec(std::string("SELECT ") + MESSAGE_COLS + " FROM messages WHERE author = ? AND nonce = ?", {member, nonce}, &r) ||
            r.rows.empty())
            return server_error();
        return {200, message_json(r.rows[0])};
    }
    Result r;
    if (!db->exec(std::string("SELECT ") + MESSAGE_COLS + " FROM messages WHERE id = ?", {ins.insert_id}, &r) || r.rows.empty()) {
        db->rollback();
        return server_error();
    }
    std::string msg = message_json(r.rows[0]);
    if (!add_event(*db, "message", msg) || !db->commit()) {
        db->rollback();
        return server_error();
    }
    return {200, msg};
}

Answer Service::members()
{
    auto db = pool_.get();
    Result r;
    if (!db || !db->exec(std::string("SELECT ") + MEMBER_COLS + " FROM members WHERE left_ms IS NULL ORDER BY joined_ms", {}, &r))
        return server_error();
    std::string out = "{\"members\":[";
    for (size_t i = 0; i < r.rows.size(); i++)
        out += (i ? "," : "") + member_json(r.rows[i]);
    return {200, out + "]}"};
}

Answer Service::set_profile(const std::string &member, const std::string &profile)
{
    auto db = pool_.get();
    Result m;
    if (!db || !db->begin())
        return server_error();
    bool ok = db->exec("UPDATE members SET profile = ? WHERE id = ? AND left_ms IS NULL",
                       {profile.empty() ? Value(nullptr) : Value(profile), member}) &&
              db->exec(std::string("SELECT ") + MEMBER_COLS + " FROM members WHERE id = ?", {member}, &m) && !m.rows.empty() &&
              add_event(*db, "member", member_json(m.rows[0])) && db->commit();
    if (!ok) {
        db->rollback();
        return server_error();
    }
    return {204, ""};
}

static bool remove_member(Db &db, const std::string &id, Service *s, bool (Service::*add)(Db &, const char *, const std::string &))
{
    Result r;
    if (!db.begin())
        return false;
    bool ok = db.exec("UPDATE members SET left_ms = ? WHERE id = ? AND left_ms IS NULL", {now_ms(), id}, &r) &&
              (r.affected == 0 || (s->*add)(db, "member_left", "{\"id\":" + jstr(id) + "}")) && db.commit();
    if (!ok)
        db.rollback();
    return ok;
}

Answer Service::leave(const std::string &member)
{
    auto db = pool_.get();
    if (!db || !remove_member(*db, member, this, &Service::add_event))
        return server_error();
    puddle_log(PUDDLE_LOG_INFO, "%s left", member.c_str());
    return {204, ""};
}

/* ---- the event log ---- */

bool Service::add_event(Db &db, const char *kind, const std::string &data)
{
    Result r;
    if (!db.exec("INSERT INTO events (kind, data, ts_ms) VALUES (?, ?, ?)", {std::string(kind), data, now_ms()}, &r))
        return false;
    bump(r.insert_id); /* early: a waiter that wakes before the commit finds nothing and waits again */
    return true;
}

void Service::bump(int64_t id)
{
    std::lock_guard<std::mutex> g(mu_);
    if (id > latest_) {
        latest_ = id;
        cv_.notify_all();
    }
}

Answer Service::events(int64_t after)
{
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
    for (;;) {
        {
            auto db = pool_.get();
            Result r;
            if (!db || !db->exec("SELECT id, kind, data FROM events WHERE id > ? ORDER BY id LIMIT ?", {after, (int64_t)EVENTS_MAX}, &r))
                return server_error();
            int64_t oldest;
            {
                std::lock_guard<std::mutex> g(mu_);
                oldest = oldest_;
            }
            /* the cursor is from before the oldest event kept: what came between is gone, so start again from /info */
            if (oldest > 0 && after + 1 < oldest && (r.rows.empty() || num(r.rows[0][0]) == oldest))
                return {200, "{\"reset\":true}"};
            if (!r.rows.empty()) {
                std::string out = "{\"events\":[";
                for (size_t i = 0; i < r.rows.size(); i++)
                    out += std::string(i ? "," : "") + "{\"id\":" + std::to_string(num(r.rows[i][0])) + ",\"kind\":" + jstr(text(r.rows[i][1])) +
                           ",\"data\":" + text(r.rows[i][2]) + "}";
                return {200, out + "],\"cursor\":" + std::to_string(num(r.rows.back()[0])) + "}"};
            }
        } /* the connection goes back to the pool while this waits */
        std::unique_lock<std::mutex> lk(mu_);
        if (!cv_.wait_until(lk, deadline, [&] { return latest_ > after || stopping_; }) || stopping_)
            return {200, "{\"events\":[],\"cursor\":" + std::to_string(after) + "}"};
    }
}

void Service::watch()
{
    int64_t next_prune = 0;
    while (!stopping_) {
        {
            auto db = pool_.get();
            Result r;
            if (db && now_ms() >= next_prune) {
                db->exec("DELETE FROM events WHERE ts_ms < ?", {now_ms() - EVENTS_KEEP_MS});
                next_prune = now_ms() + 3600 * 1000;
            }
            if (db && db->exec("SELECT COALESCE(MAX(id), 0), COALESCE(MIN(id), 0) FROM events", {}, &r) && !r.rows.empty()) {
                bump(num(r.rows[0][0]));
                std::lock_guard<std::mutex> g(mu_);
                oldest_ = num(r.rows[0][1]);
            }
        }
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait_for(lk, std::chrono::milliseconds(500), [&] { return stopping_.load(); });
    }
}

void Service::stop()
{
    std::lock_guard<std::mutex> g(mu_);
    stopping_ = true;
    cv_.notify_all();
}

/* ---- the admin's ---- */

bool Service::set_name(const std::string &name, const std::string *description, std::string &err)
{
    std::string n, d;
    if (!clean(name, POND_NAME_MAX, false, n) || n.empty()) {
        err = "a name is 1 to " + std::to_string(POND_NAME_MAX) + " characters";
        return false;
    }
    if (description && !clean(*description, DESCRIPTION_MAX, true, d)) {
        err = "a description is at most " + std::to_string(DESCRIPTION_MAX) + " characters";
        return false;
    }
    auto db = pool_.get();
    bool ok = db && db->begin() && db->exec("REPLACE INTO settings (name, value) VALUES ('name', ?)", {n}) &&
              (!description || db->exec("REPLACE INTO settings (name, value) VALUES ('description', ?)", {d})) &&
              add_event(*db, "pond", pond_json(*db)) && db->commit();
    if (!ok) {
        if (db)
            db->rollback();
        err = "the database refused it";
    }
    return ok;
}

bool Service::channel_create(const std::string &name, const std::string &topic, std::string &err)
{
    std::string t;
    if (!channel_name_ok(name)) {
        err = "a channel name is 1 to 32 of a-z, 0-9 and -";
        return false;
    }
    if (!clean(topic, TOPIC_MAX, false, t)) {
        err = "a topic is at most " + std::to_string(TOPIC_MAX) + " characters";
        return false;
    }
    auto db = pool_.get();
    Result r, c;
    if (!db || !db->begin())
        return err = "no database", false;
    bool ok = db->exec("INSERT INTO channels (name, topic, position, created_ms)"
                       " SELECT ?, ?, COALESCE(MAX(position), -1) + 1, ? FROM channels WHERE deleted = 0",
                       {name, t, now_ms()}, &r) &&
              db->exec("SELECT id, name, topic, position FROM channels WHERE id = ?", {r.insert_id}, &c) && !c.rows.empty() &&
              add_event(*db, "channel", channel_json(c.rows[0])) && db->commit();
    if (!ok) {
        err = db->last_errno() == 1062 ? "there is already a channel called " + name : "the database refused it";
        db->rollback();
    }
    return ok;
}

/* Change one column of a live channel and tell the members. */
static bool channel_set(Pool &pool, const std::string &name, const char *column, const std::string &value, std::string &err,
                        const std::function<bool(Db &, const std::string &)> &tell)
{
    auto db = pool.get();
    Result r, c;
    if (!db || !db->begin())
        return err = "no database", false;
    bool ok = db->exec(std::string("UPDATE channels SET ") + column + " = ? WHERE name = ? AND deleted = 0", {value, name}, &r);
    if (ok && r.affected == 0) {
        db->rollback();
        Result e;
        err = db->exec("SELECT 1 FROM channels WHERE name = ? AND deleted = 0", {name}, &e) && !e.rows.empty() ? "" : "no channel called " + name;
        return err.empty();
    }
    ok = ok && db->exec("SELECT id, name, topic, position FROM channels WHERE name = ? AND deleted = 0",
                        {std::string(column) == "name" ? value : name}, &c) &&
         !c.rows.empty() && tell(*db, channel_json(c.rows[0])) && db->commit();
    if (!ok) {
        err = db->last_errno() == 1062 ? "there is already a channel called " + value : "the database refused it";
        db->rollback();
    }
    return ok;
}

bool Service::channel_rename(const std::string &name, const std::string &to, std::string &err)
{
    if (!channel_name_ok(to)) {
        err = "a channel name is 1 to 32 of a-z, 0-9 and -";
        return false;
    }
    return channel_set(pool_, name, "name", to, err, [this](Db &db, const std::string &j) { return add_event(db, "channel", j); });
}

bool Service::channel_topic(const std::string &name, const std::string &topic, std::string &err)
{
    std::string t;
    if (!clean(topic, TOPIC_MAX, false, t)) {
        err = "a topic is at most " + std::to_string(TOPIC_MAX) + " characters";
        return false;
    }
    return channel_set(pool_, name, "topic", t, err, [this](Db &db, const std::string &j) { return add_event(db, "channel", j); });
}

bool Service::channel_delete(const std::string &name, std::string &err)
{
    auto db = pool_.get();
    int64_t id;
    if (!db || !db->begin())
        return err = "no database", false;
    if (!live_channel(*db, name, id, nullptr, true)) {
        db->rollback();
        err = "no channel called " + name;
        return false;
    }
    bool ok = db->exec("UPDATE channels SET deleted = 1 WHERE id = ?", {id}) &&
              add_event(*db, "channel_deleted", "{\"id\":" + std::to_string(id) + "}") && db->commit();
    if (!ok) {
        db->rollback();
        err = "the database refused it";
    }
    return ok;
}

bool Service::invite_create(int max_uses, int64_t expires_ms, std::string &token, std::string &err)
{
    auto db = pool_.get();
    token = new_token();
    if (!db || !db->exec("INSERT INTO invites (token, max_uses, expires_ms, created_ms) VALUES (?, ?, ?, ?)",
                         {token, max_uses > 0 ? Value((int64_t)max_uses) : Value(nullptr), expires_ms > 0 ? Value(expires_ms) : Value(nullptr),
                          now_ms()})) {
        err = "the database refused it";
        return false;
    }
    return true;
}

bool Service::invite_revoke(const std::string &token, std::string &err)
{
    auto db = pool_.get();
    Result r;
    if (!db || !db->exec("UPDATE invites SET revoked = 1 WHERE token = ?", {token}, &r)) {
        err = "the database refused it";
        return false;
    }
    if (r.affected == 0) {
        Result e;
        if (!db->exec("SELECT 1 FROM invites WHERE token = ?", {token}, &e) || e.rows.empty()) {
            err = "no invite " + token;
            return false;
        }
    }
    return true;
}

bool Service::member_remove(const std::string &id, std::string &err)
{
    if (!is_member(id)) {
        err = "no member " + id;
        return false;
    }
    auto db = pool_.get();
    if (!db || !remove_member(*db, id, this, &Service::add_event)) {
        err = "the database refused it";
        return false;
    }
    return true;
}

} // namespace pond
