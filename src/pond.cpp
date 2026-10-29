#include "pond.h"

#include "encoding.h"
#include "json.h"
#include "text.h"

#include <drogon/orm/Exception.h>
#include <trantor/utils/Logger.h>

#include <cctype>
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
    : db_(std::move(db)), key_(std::move(key)), files_dir_(data_dir + "/files"), timers_(timers)
{
}

/* ---- JSON for what is kept ---- */

static const char MEMBER_COLS[] = "id, profile, joined_ms";
static const char CHANNEL_COLS[] = "id, name, topic, position";
static const char MESSAGE_COLS[] = "id, channel, author, text, ts_ms";

static std::string member_json(const drogon::orm::Row &row) /* MEMBER_COLS */
{
    return "{\"id\":" + jstr(text(row[0])) + ",\"profile\":" + (row[1].isNull() ? std::string("null") : text(row[1])) +
           ",\"joined\":" + std::to_string(num(row[2])) + "}";
}

static std::string channel_json(const drogon::orm::Row &row) /* CHANNEL_COLS */
{
    return "{\"id\":" + std::to_string(num(row[0])) + ",\"name\":" + jstr(text(row[1])) + ",\"topic\":" + jstr(text(row[2])) +
           ",\"position\":" + std::to_string(num(row[3])) + "}";
}

/* `files` is the message's "files" array, or "" for a message without any */
static std::string message_json(const drogon::orm::Row &row, const std::string &files = "") /* MESSAGE_COLS */
{
    return "{\"id\":" + std::to_string(num(row[0])) + ",\"channel\":" + std::to_string(num(row[1])) + ",\"author\":" + jstr(text(row[2])) +
           ",\"text\":" + jstr(text(row[3])) + ",\"ts\":" + std::to_string(num(row[4])) + (files.empty() ? "" : ",\"files\":" + files) + "}";
}

/* Rows of MESSAGE_COLS as a JSON array, each with the files it carries. */
Task<std::string> Service::messages_json(const DbPtr &db, const Result &r)
{
    std::map<int64_t, std::string> files;
    if (!r.empty()) {
        std::string ids;
        for (size_t i = 0; i < r.size(); i++)
            ids += (i ? "," : "") + std::to_string(num(r[i][0])); /* numbers from the database: safe to put in the statement */
        Result f = co_await db->execSqlCoro("SELECT message, file, name, mime, size, w, h, duration_ms, preview FROM message_files"
                                            " WHERE message IN (" + ids + ") ORDER BY message, position");
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
    }
    std::string out = "[";
    for (size_t i = 0; i < r.size(); i++) {
        auto it = files.find(num(r[i][0]));
        out += (i ? "," : "") + message_json(r[i], it == files.end() ? "" : it->second);
    }
    co_return out + "]";
}

Task<std::string> Service::member_json_of(const DbPtr &db, const std::string &id)
{
    Result r = co_await db->execSqlCoro(std::string("SELECT ") + MEMBER_COLS + " FROM members WHERE id = ?", id);
    co_return r.empty() ? "" : member_json(r[0]);
}

Task<std::string> Service::pond_json(const DbPtr &db)
{
    Result r = co_await db->execSqlCoro("SELECT name, value FROM settings WHERE name IN ('name', 'description', 'files.max_size')");
    std::string name, description, max = std::to_string(FILES_DEFAULT_MAX);
    for (const auto &row : r)
        (text(row[0]) == "name" ? name : text(row[0]) == "description" ? description : max) = text(row[1]);
    /* what members may send: "files" (docs/design.md, "Files"); max_size 0 is files turned off */
    co_return "{\"id\":" + jstr(key_.id) + ",\"name\":" + jstr(name) + ",\"description\":" + jstr(description) +
              ",\"files\":{\"max_size\":" + std::to_string(strtoll(max.c_str(), nullptr, 10)) + ",\"chunk_size\":" +
              std::to_string(FILE_CHUNK) + ",\"per_message\":" + std::to_string(FILES_PER_MESSAGE) + "}}";
}

Task<std::string> Service::channels_json()
{
    Result r = co_await db_->execSqlCoro(std::string("SELECT ") + CHANNEL_COLS + " FROM channels WHERE deleted = 0 ORDER BY position, id");
    std::string out = "[";
    for (size_t i = 0; i < r.size(); i++)
        out += (i ? "," : "") + channel_json(r[i]);
    co_return out + "]";
}

Task<std::string> Service::setting(std::string name)
{
    Result r = co_await db_->execSqlCoro("SELECT value FROM settings WHERE name = ?", name);
    co_return r.empty() ? "" : text(r[0][0]);
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
    co_return a.status == 200 ? co_await info(chain.id) : a;
}

Task<Answer> Service::enter(Chain chain, std::string invite, std::string profile)
{
    TxPtr tx;
    try {
        tx = co_await db_->newTransactionCoro();
        Result r = co_await tx->execSqlCoro("SELECT left_ms FROM members WHERE id = ? FOR UPDATE", chain.id);
        bool member = !r.empty() && r[0][0].isNull();
        int64_t now = now_ms();
        if (!member) {
            /* spend one use, in the same statement that checks the invite may still be used, so two joins can't share the last */
            Result u = co_await tx->execSqlCoro("UPDATE invites SET uses = uses + 1 WHERE token = ? AND revoked = 0"
                                                " AND (max_uses IS NULL OR uses < max_uses) AND (expires_ms IS NULL OR expires_ms > ?)",
                                                invite, now);
            if (u.affectedRows() != 1) {
                tx->rollback();
                co_return fail(404, "not_found");
            }
        }
        co_await tx->execSqlCoro("INSERT INTO members (id, sign_pub, kid, seq, chain, profile, joined_ms) VALUES (?, ?, ?, ?, ?, ?, ?)"
                                 " ON DUPLICATE KEY UPDATE sign_pub = VALUES(sign_pub), kid = VALUES(kid), seq = VALUES(seq),"
                                 " chain = VALUES(chain), profile = VALUES(profile),"
                                 " joined_ms = IF(left_ms IS NULL, joined_ms, VALUES(joined_ms)), left_ms = NULL",
                                 chain.id, chain.sign_pub_hex, chain.kid, (int64_t)chain.seq, chain.text,
                                 profile.empty() ? std::nullopt : std::optional<std::string>(profile), now);
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
        co_return Answer{200, "{\"protocol\":" + std::to_string(PROTOCOL) + ",\"features\":" + features + "],\"pond\":" +
                                  co_await pond_json(db_) + ",\"me\":" + me + ",\"channels\":" + co_await channels_json() +
                                  ",\"cursor\":" + std::to_string(num(c[0][0])) + "}"};
    } catch (const DrogonDbException &e) {
        co_return server_error(e);
    }
}

Task<Answer> Service::history(int64_t channel, int64_t before, int limit)
{
    try {
        Result live = co_await db_->execSqlCoro("SELECT 1 FROM channels WHERE id = ? AND deleted = 0", channel);
        if (live.empty())
            co_return fail(404, "no_channel");
        Result r = co_await db_->execSqlCoro(std::string("SELECT ") + MESSAGE_COLS +
                                                 " FROM messages WHERE channel = ? AND id < ? ORDER BY id DESC LIMIT ?",
                                             channel, before > 0 ? before : INT64_MAX, (int64_t)limit);
        co_return Answer{200, "{\"messages\":" + co_await messages_json(db_, r) + "}"};
    } catch (const DrogonDbException &e) {
        co_return server_error(e);
    }
}

/* What a message says of a file, cleaned: false if it cannot be. */
static bool clean_file(FileRef &f)
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

Task<Answer> Service::send(std::string member, int64_t channel, std::string in, std::string nonce, std::vector<FileRef> files)
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
    TxPtr tx;
    try {
        Result live = co_await db_->execSqlCoro("SELECT 1 FROM channels WHERE id = ? AND deleted = 0", channel);
        if (live.empty())
            co_return fail(404, "no_channel");
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
        Result ins = co_await tx->execSqlCoro("INSERT INTO messages (channel, author, text, nonce, ts_ms) VALUES (?, ?, ?, ?, ?)", channel,
                                              member, body, nonce.empty() ? std::nullopt : std::optional<std::string>(nonce), now_ms());
        int64_t mid = (int64_t)ins.insertId();
        for (size_t i = 0; i < files.size(); i++) {
            const FileRef &f = files[i];
            auto opt = [](int64_t v) { return v > 0 ? std::optional<int64_t>(v) : std::nullopt; };
            co_await tx->execSqlCoro("INSERT INTO message_files (message, position, file, name, mime, size, w, h, duration_ms, preview)"
                                     " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                                     mid, (int64_t)i, f.id, f.name, f.mime, sizes[i], opt(f.w), opt(f.h), opt(f.duration_ms),
                                     f.preview.empty() ? std::nullopt : std::optional<std::string>(f.preview));
        }
        Result r = co_await tx->execSqlCoro(std::string("SELECT ") + MESSAGE_COLS + " FROM messages WHERE id = ?", mid);
        std::string list = co_await messages_json(tx, r);
        std::string msg = list.substr(1, list.size() - 2);
        if (msg.empty())
            throw std::out_of_range("message");
        int64_t ev = co_await add_event(tx, "message", msg);
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
        co_return r.empty() ? fail(500, "database") : Answer{200, list.substr(1, list.size() - 2)};
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
        co_await tx->execSqlCoro("UPDATE members SET profile = ? WHERE id = ? AND left_ms IS NULL",
                                 profile.empty() ? std::nullopt : std::optional<std::string>(profile), member);
        int64_t ev = co_await add_event(tx, "member", co_await member_json_of(tx, member));
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
        Result r = co_await tx->execSqlCoro("UPDATE members SET left_ms = ? WHERE id = ? AND left_ms IS NULL", now_ms(), id);
        int64_t ev = r.affectedRows() ? co_await add_event(tx, "member_left", "{\"id\":" + jstr(id) + "}") : 0;
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

/* ---- the event log ---- */

Task<int64_t> Service::add_event(const TxPtr &tx, const char *kind, const std::string &data)
{
    co_await tx->execSqlCoro("SELECT value FROM settings WHERE name = 'event_lock' FOR UPDATE");
    Result r = co_await tx->execSqlCoro("INSERT INTO events (kind, data, ts_ms) VALUES (?, ?, ?)", std::string(kind), data, now_ms());
    co_return (int64_t)r.insertId();
}

/* One request for events, waiting. Whoever sets `done` first (news, the time running out, or stopping) resumes it. */
struct Service::Waiter {
    int64_t after;
    std::coroutine_handle<> h;
    std::atomic<bool> done{false};
    void wake()
    {
        if (!done.exchange(true))
            h.resume();
    }
};

struct Service::News {
    Service *s;
    int64_t after;
    double seconds;

    bool await_ready()
    {
        std::lock_guard<std::mutex> g(s->mu_);
        return s->latest_ > after || s->stopping_;
    }
    bool await_suspend(std::coroutine_handle<> h)
    {
        auto w = std::make_shared<Waiter>();
        w->after = after;
        w->h = h;
        {
            std::lock_guard<std::mutex> g(s->mu_);
            if (s->latest_ > after || s->stopping_)
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

Task<Answer> Service::events(int64_t after)
{
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
    for (;;) {
        try {
            Result r = co_await db_->execSqlCoro("SELECT id, kind, data FROM events WHERE id > ? ORDER BY id LIMIT ?", after, (int64_t)EVENTS_MAX);
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
            if (!r.empty()) {
                std::string out = "{\"events\":[";
                for (size_t i = 0; i < r.size(); i++)
                    out += std::string(i ? "," : "") + "{\"id\":" + std::to_string(num(r[i][0])) + ",\"kind\":" + jstr(text(r[i][1])) +
                           ",\"data\":" + text(r[i][2]) + "}";
                co_return Answer{200, out + "],\"cursor\":" + std::to_string(num(r[r.size() - 1][0])) + "}"};
            }
        } catch (const DrogonDbException &e) {
            co_return server_error(e);
        }
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0 || stopping_ || !timers_)
            break;
        co_await News{this, after, (double)left / 1000.0};
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
        int64_t ev = co_await add_event(tx, "pond", co_await pond_json(tx));
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

Task<Outcome> Service::channel_create(std::string name, std::string topic)
{
    std::string t;
    if (!channel_name_ok(name))
        co_return "a channel name is 1 to 32 of a-z, 0-9 and -";
    if (!clean_text(topic, TOPIC_MAX, false, t))
        co_return "a topic is at most " + std::to_string(TOPIC_MAX) + " characters";
    TxPtr tx;
    try {
        tx = co_await db_->newTransactionCoro();
        Result r = co_await tx->execSqlCoro("INSERT INTO channels (name, topic, position, created_ms)"
                                            " SELECT ?, ?, COALESCE(MAX(position), -1) + 1, ? FROM channels WHERE deleted = 0",
                                            name, t, now_ms());
        Result c = co_await tx->execSqlCoro(std::string("SELECT ") + CHANNEL_COLS + " FROM channels WHERE id = ?", (int64_t)r.insertId());
        int64_t ev = co_await add_event(tx, "channel", channel_json(c.at(0)));
        if (!co_await Committed(std::move(tx)))
            co_return "the database did not commit it";
        bump(ev);
        co_return "";
    } catch (const DrogonDbException &e) {
        if (tx)
            tx->rollback();
        co_return sql_errno(e) == 1062 ? "there is already a channel called " + name : refused(e);
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
            co_return e.empty() ? "no channel called " + name : "";
        }
        Result c = co_await tx->execSqlCoro(std::string("SELECT ") + CHANNEL_COLS + " FROM channels WHERE name = ? AND deleted = 0",
                                            std::string(column) == "name" ? value : name);
        int64_t ev = co_await add_event(tx, "channel", channel_json(c.at(0)));
        if (!co_await Committed(std::move(tx)))
            co_return "the database did not commit it";
        bump(ev);
        co_return "";
    } catch (const DrogonDbException &e) {
        if (tx)
            tx->rollback();
        co_return sql_errno(e) == 1062 ? "there is already a channel called " + value : refused(e);
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
        co_await db_->execSqlCoro("INSERT INTO invites (token, max_uses, expires_ms, created_ms) VALUES (?, ?, ?, ?)", *token,
                                  max_uses > 0 ? std::optional<int64_t>(max_uses) : std::nullopt,
                                  expires_ms > 0 ? std::optional<int64_t>(expires_ms) : std::nullopt, now_ms());
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
