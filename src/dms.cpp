/* Direct messages between members (docs/dms.md): the Pond relays letters, sealed from one member's Puddle to another's, and
 * keeps none of them. A letter goes to the recipient's Puddle on the request it holds open on the events, from memory, and is
 * never written to the events table; a recipient whose Puddle isn't following now is told about to its senders when it is
 * back (`dm_back`), and they send again. A letter's files are passed the same way, a piece at a time: the recipient asks,
 * the sender is told (`dm_want`) and sends that piece, and the Pond hands it over. All of it lives in pondd's memory, under
 * the Service's lock, and is gone with a restart, which senders allow for by trying again. */

#include "pond.h"

#include "json.h"

#include <drogon/orm/Exception.h>
#include <trantor/utils/Logger.h>

namespace pond {

using drogon::orm::DrogonDbException;
using drogon::orm::Result;

namespace {

int64_t day_of(int64_t ms)
{
    return ms / (24LL * 3600 * 1000);
}

/* The key a piece is waited for under: who asks, who sends, the letter, the file and the piece. */
std::string piece_key(const std::string &to, const std::string &from, const std::string &letter, int n, const std::string &part)
{
    return to + "/" + from + "/" + letter + "/" + std::to_string(n) + "/" + part;
}

} // namespace

/* A request for a piece of a file, waiting for the sender's Puddle to send it. Whoever sets `done` first (the piece, or the
 * time running out) resumes it. */
struct Service::Piece {
    std::coroutine_handle<> h;
    std::atomic<bool> done{false};
    bool got = false;
    std::string bytes;
    void wake()
    {
        if (!done.exchange(true))
            h.resume();
    }
};

struct Service::PieceWait {
    std::shared_ptr<Piece> p;
    trantor::EventLoop *timers;
    double seconds;
    bool await_ready() { return p->done; }
    bool await_suspend(std::coroutine_handle<> h)
    {
        p->h = h;
        auto keep = p;
        timers->runAfter(seconds, [keep] { keep->wake(); });
        return true;
    }
    void await_resume() {}
};

bool Service::present_locked(const std::string &member, int64_t now) const
{
    auto it = seen_.find(member);
    return it != seen_.end() && (it->second.open > 0 || now - it->second.last_ms <= DM_PRESENT_MS);
}

void Service::push_locked(const std::string &member, const std::string &kind, const std::string &data,
                          std::vector<std::shared_ptr<Waiter>> &wake)
{
    auto &box = inbox_[member];
    if (box.size() >= DM_INBOX_MAX)
        box.pop_front(); /* a Puddle that isn't taking them: the oldest go, and their senders try again */
    box.push_back("{\"kind\":" + jstr(kind) + ",\"data\":" + data + "}");
    std::erase_if(waiters_, [&](const std::shared_ptr<Waiter> &w) {
        if (w->done)
            return true;
        if (w->member != member)
            return false;
        wake.push_back(w);
        return true;
    });
}

void Service::following(const std::string &member, bool open)
{
    std::vector<std::shared_ptr<Waiter>> wake;
    {
        std::lock_guard<std::mutex> g(mu_);
        const int64_t now = now_ms();
        if (open && !present_locked(member, now)) {
            /* back: those who wanted to write to them try again now */
            auto it = awaiting_.find(member);
            if (it != awaiting_.end()) {
                const std::string data = "{\"member\":" + jstr(member) + "}";
                for (const std::string &sender : it->second)
                    push_locked(sender, "dm_back", data, wake);
                awaiting_.erase(it);
            }
        }
        Seen &s = seen_[member];
        s.open += open ? 1 : -1;
        if (s.open < 0)
            s.open = 0;
        s.last_ms = now;
    }
    for (auto &w : wake)
        w->wake();
}

std::vector<std::string> Service::take_inbox(const std::string &member)
{
    std::lock_guard<std::mutex> g(mu_);
    auto it = inbox_.find(member);
    if (it == inbox_.end())
        return {};
    std::vector<std::string> out(it->second.begin(), it->second.end());
    inbox_.erase(it);
    return out;
}

bool Service::has_inbox_locked(const std::string &member) const
{
    auto it = inbox_.find(member);
    return it != inbox_.end() && !it->second.empty();
}

void Service::forget_dms(const std::string &member)
{
    std::lock_guard<std::mutex> g(mu_);
    inbox_.erase(member);
    awaiting_.erase(member);
    for (auto &[to, senders] : awaiting_)
        senders.erase(member);
}

Task<Service::DmSettings> Service::dm_settings()
{
    Result r = co_await db_->execSqlCoro("SELECT name, value FROM settings WHERE name IN ('dms', 'dms.files_max')");
    DmSettings s;
    for (const auto &row : r) {
        if (text(row[0]) == "dms")
            s.on = text(row[1]) != "off";
        else
            s.files_max = strtoll(text(row[1]).c_str(), nullptr, 10);
    }
    co_return s;
}

Task<Answer> Service::member_chain(std::string member)
{
    try {
        Result r = co_await db_->execSqlCoro("SELECT chain FROM members WHERE id = ? AND left_ms IS NULL", member);
        if (r.empty())
            co_return fail(404, "no_member");
        co_return Answer{200, "{\"id\":" + jstr(member) + ",\"chain\":" + jstr(text(r[0][0])) + "}"};
    } catch (const DrogonDbException &e) {
        LOG_ERROR << "database: " << e.base().what();
        co_return fail(500, "database");
    }
}

/* Both are members now, and `to` takes direct messages: "" if so, else the error code. */
Task<std::string> Service::may_write(std::string from, std::string to)
{
    if (from == to)
        co_return "no_member";
    const DmSettings s = co_await dm_settings();
    if (!s.on)
        co_return "dms_off";
    Result r = co_await db_->execSqlCoro("SELECT id, dms FROM members WHERE id IN (?, ?) AND left_ms IS NULL", from, to);
    bool have_from = false, have_to = false, accepts = false;
    for (const auto &row : r) {
        if (text(row[0]) == from)
            have_from = true;
        if (text(row[0]) == to)
            have_to = true, accepts = num(row[1]) != 0;
    }
    if (!have_from || !have_to)
        co_return "no_member";
    if (!accepts)
        co_return "not_accepted";
    co_return "";
}

Task<Answer> Service::dm_send(std::string from, std::string to, std::string id, std::string kid, std::string enc, std::string sealed)
{
    try {
        const std::string why = co_await may_write(from, to);
        const int status = why == "no_member" ? 404 : 409;
        if (!why.empty())
            co_return fail(status, why.c_str());
    } catch (const DrogonDbException &e) {
        LOG_ERROR << "database: " << e.base().what();
        co_return fail(500, "database");
    }
    const int64_t now = now_ms();
    if (!dm_limit_.take(from, now))
        co_return fail(429, "slow_down");
    std::vector<std::shared_ptr<Waiter>> wake;
    bool relayed;
    {
        std::lock_guard<std::mutex> g(mu_);
        /* someone new to them, who hasn't written to them first: at most DM_NEW_PER_DAY a day (docs/dms.md, "Limits") */
        if (!wrote_.count(to + " " + from)) {
            NewToday &n = new_today_[from];
            if (n.day != day_of(now))
                n.day = day_of(now), n.to.clear();
            if (!n.to.count(to)) {
                if (n.to.size() >= DM_NEW_PER_DAY)
                    co_return fail(429, "too_many_new");
                n.to.insert(to);
            }
        }
        if (wrote_.size() < DM_PAIRS_MAX)
            wrote_.insert(from + " " + to);
        relayed = present_locked(to, now);
        if (relayed)
            push_locked(to, "dm",
                        "{\"id\":" + jstr(id) + ",\"from\":" + jstr(from) + ",\"kid\":" + jstr(kid) + ",\"enc\":" + jstr(enc) +
                            ",\"sealed\":" + jstr(sealed) + ",\"ts\":" + std::to_string(now) + "}",
                        wake);
        else if (awaiting_[to].size() < DM_AWAITING_MAX)
            awaiting_[to].insert(from);
    }
    for (auto &w : wake)
        w->wake();
    const std::string said = relayed ? "true" : "false";
    co_return Answer{200, "{\"id\":" + jstr(id) + ",\"relayed\":" + said + "}"};
}

Task<Answer> Service::dm_accept(std::string member, bool accept)
{
    TxPtr tx;
    try {
        tx = co_await db_->newTransactionCoro();
        const int value = accept ? 1 : 0;
        Result r = co_await tx->execSqlCoro("UPDATE members SET dms = ? WHERE id = ? AND left_ms IS NULL", value, member);
        int64_t ev = 0;
        if (r.affectedRows()) { /* everyone's app then offers Message to them, or doesn't */
            const std::string mj = co_await member_json_of(tx, member);
            ev = co_await add_event(tx, "member", mj);
        }
        if (!co_await Committed(std::move(tx)))
            co_return fail(500, "database");
        bump(ev);
        const std::string said = accept ? "true" : "false";
        co_return Answer{200, "{\"accept\":" + said + "}"};
    } catch (const DrogonDbException &e) {
        if (tx)
            tx->rollback();
        LOG_ERROR << "database: " << e.base().what();
        co_return fail(500, "database");
    }
}

/* A piece is "hashes" (the file's chunk hashes) or a chunk's number; `n` the file's place in the letter. */
static bool part_ok(const std::string &part, int64_t files_max)
{
    if (part == "hashes")
        return true;
    if (part.empty() || part.size() > 9 || (part[0] == '0' && part.size() > 1))
        return false;
    for (char c : part)
        if (c < '0' || c > '9')
            return false;
    return strtoll(part.c_str(), nullptr, 10) * (int64_t)DM_CHUNK < files_max;
}

Task<Answer> Service::dm_piece_get(std::string to, std::string from, std::string letter, int n, std::string part)
{
    try {
        const DmSettings s = co_await dm_settings();
        if (s.files_max <= 0)
            co_return fail(409, "files_off");
        if (!part_ok(part, s.files_max))
            co_return fail(400, "bad_part");
        /* the sender must still be one who may write to them */
        const std::string why = co_await may_write(from, to);
        const int status = why == "no_member" ? 404 : 409;
        if (!why.empty())
            co_return fail(status, why.c_str());
    } catch (const DrogonDbException &e) {
        LOG_ERROR << "database: " << e.base().what();
        co_return fail(500, "database");
    }
    if (!piece_limit_.take(to, now_ms()))
        co_return fail(429, "slow_down");
    auto p = std::make_shared<Piece>();
    const std::string key = piece_key(to, from, letter, n, part);
    std::vector<std::shared_ptr<Waiter>> wake;
    {
        std::lock_guard<std::mutex> g(mu_);
        if (pieces_.size() >= DM_PIECES_MAX || pieces_.count(key))
            co_return fail(429, "slow_down");
        if (!present_locked(from, now_ms()))
            co_return fail(409, "away"); /* their Puddle isn't following: ask again once it writes */
        pieces_[key] = p;
        push_locked(from, "dm_want",
                    "{\"to\":" + jstr(to) + ",\"letter\":" + jstr(letter) + ",\"file\":" + std::to_string(n) + ",\"part\":" + jstr(part) + "}",
                    wake);
    }
    for (auto &w : wake)
        w->wake();
    if (timers_)
        co_await PieceWait{p, timers_, DM_PIECE_WAIT_S};
    {
        std::lock_guard<std::mutex> g(mu_);
        auto it = pieces_.find(key);
        if (it != pieces_.end() && it->second == p)
            pieces_.erase(it);
    }
    if (!p->got)
        co_return fail(409, "away");
    co_return Answer{200, std::move(p->bytes), true};
}

Task<Answer> Service::dm_piece_put(std::string from, std::string to, std::string letter, int n, std::string part, std::string bytes)
{
    std::shared_ptr<Piece> p;
    {
        std::lock_guard<std::mutex> g(mu_);
        auto it = pieces_.find(piece_key(to, from, letter, n, part));
        if (it == pieces_.end())
            co_return fail(404, "not_asked"); /* nobody waits for it (any more) */
        p = it->second;
        pieces_.erase(it);
    }
    if (p->done)
        co_return fail(404, "not_asked");
    p->bytes = std::move(bytes);
    p->got = true;
    p->wake();
    co_return Answer{204, ""};
}

Task<Outcome> Service::set_dms(std::optional<bool> on, std::optional<int64_t> files_max)
{
    if (files_max && (*files_max < 0 || *files_max > DM_FILES_HARD_MAX))
        co_return "the largest file in a direct message is at most " + std::to_string(DM_FILES_HARD_MAX >> 20) + " MiB";
    TxPtr tx;
    try {
        tx = co_await db_->newTransactionCoro();
        if (on)
        {
            const std::string value = *on ? "on" : "off";
            co_await tx->execSqlCoro("REPLACE INTO settings (name, value) VALUES ('dms', ?)", value);
        }
        if (files_max)
            co_await tx->execSqlCoro("REPLACE INTO settings (name, value) VALUES ('dms.files_max', ?)", std::to_string(*files_max));
        std::string pj = co_await pond_json(tx); /* one co_await per statement (docs/design.md, "Coroutines") */
        int64_t ev = co_await add_event(tx, "pond", pj);
        if (!co_await Committed(std::move(tx)))
            co_return "the database did not commit it";
        bump(ev);
        co_return "";
    } catch (const DrogonDbException &e) {
        if (tx)
            tx->rollback();
        co_return std::string("database: ") + e.base().what();
    }
}

} // namespace pond
