/* Roles (docs/design.md, "Roles"): operators and administrators, who run the Pond from the app. A role is offered from pondctl
 * and accepted by the member in their app; holding one opens the admin routes, and rank decides who may act on whom. Bans,
 * and the admin log of who did what, are here too. */

#include "pond.h"

#include "json.h"
#include "text.h"

#include <drogon/orm/Exception.h>
#include <trantor/utils/Logger.h>

namespace pond {

using drogon::orm::DrogonDbException;
using drogon::orm::Result;

static Answer server_error(const DrogonDbException &e)
{
    LOG_ERROR << "database: " << e.base().what();
    return fail(500, "database");
}

/* What an admin action said, as an answer: done, or a 400 with the reason in words the app may show. */
static Answer outcome(const Outcome &o, int done = 204)
{
    if (o.empty())
        return {done, ""};
    return {400, "{\"error\":\"bad_request\",\"message\":" + jstr(o) + "}"};
}

const char *role_name(Role r)
{
    return r == Role::OPERATOR ? "operator" : r == Role::ADMINISTRATOR ? "administrator" : "member";
}

std::optional<Role> role_of_name(const std::string &s)
{
    if (s == "operator")
        return Role::OPERATOR;
    if (s == "administrator")
        return Role::ADMINISTRATOR;
    if (s == "member")
        return Role::MEMBER;
    return std::nullopt;
}

Task<Role> Service::role(std::string member)
{
    Result r = co_await db_->execSqlCoro("SELECT role FROM members WHERE id = ? AND left_ms IS NULL", member);
    co_return r.empty() ? Role::MEMBER : role_of_name(text(r[0][0])).value_or(Role::MEMBER);
}

Task<std::string> Service::offer_json(const std::string &member)
{
    Result r = co_await db_->execSqlCoro("SELECT id, role FROM role_offers WHERE member = ? AND created_ms > ? ORDER BY id DESC LIMIT 1",
                                         member, now_ms() - OFFER_KEEP_MS);
    co_return r.empty() ? "null" : "{\"id\":" + std::to_string(num(r[0][0])) + ",\"role\":" + jstr(text(r[0][1])) + "}";
}

Task<> Service::log_admin(std::string by, std::string action, std::string target, std::string detail)
{
    try {
        co_await db_->execSqlCoro("INSERT INTO admin_log (by_who, action, target, detail, ts_ms) VALUES (?, ?, ?, ?, ?)", by, action,
                                  target, detail, now_ms());
    } catch (const DrogonDbException &e) {
        LOG_ERROR << "admin log: " << e.base().what();
    }
    LOG_INFO << "admin: " << by << " " << action << " " << target << (detail.empty() ? "" : " (" + detail + ")");
}

/* ---- offers ---- */

Task<Outcome> Service::offer_role(std::string id, Role role)
{
    if (role == Role::MEMBER)
        co_return "offer operator or administrator";
    TxPtr tx;
    try {
        if (!co_await is_member(id))
            co_return "no member " + id;
        tx = co_await db_->newTransactionCoro();
        co_await tx->execSqlCoro("DELETE FROM role_offers WHERE member = ?", id);
        Result r = co_await tx->execSqlCoro("INSERT INTO role_offers (member, role, created_ms) VALUES (?, ?, ?)", id,
                                            std::string(role_name(role)), now_ms());
        std::string data = "{\"id\":" + std::to_string((int64_t)r.insertId()) + ",\"role\":" + jstr(role_name(role)) + "}";
        int64_t ev = co_await add_event(tx, "role_offer", data, id); /* for them alone */
        if (!co_await Committed(std::move(tx)))
            co_return "the database did not commit it";
        bump(ev);
    } catch (const DrogonDbException &e) {
        if (tx)
            tx->rollback();
        co_return std::string("the database refused it: ") + e.base().what();
    }
    co_await log_admin("pondctl", "offer_role", id, role_name(role));
    co_return "";
}

Task<Answer> Service::answer_offer(std::string member, int64_t offer, bool accept)
{
    TxPtr tx;
    std::string role;
    try {
        tx = co_await db_->newTransactionCoro();
        Result r = co_await tx->execSqlCoro("SELECT role FROM role_offers WHERE id = ? AND member = ? AND created_ms > ? FOR UPDATE", offer,
                                            member, now_ms() - OFFER_KEEP_MS);
        if (r.empty()) {
            tx->rollback();
            co_return fail(404, "no_offer");
        }
        role = text(r[0][0]);
        co_await tx->execSqlCoro("DELETE FROM role_offers WHERE member = ?", member);
        int64_t ev = 0;
        if (accept) {
            co_await tx->execSqlCoro("UPDATE members SET role = ? WHERE id = ? AND left_ms IS NULL", role, member);
            ev = co_await add_event(tx, "member", co_await member_json_of(tx, member));
        }
        if (!co_await Committed(std::move(tx)))
            co_return fail(500, "database");
        bump(ev);
    } catch (const DrogonDbException &e) {
        if (tx)
            tx->rollback();
        co_return server_error(e);
    }
    co_await log_admin(member, accept ? "accept_role" : "decline_role", member, role);
    co_return Answer{200, "{\"role\":" + jstr(accept ? role : std::string(role_name(co_await this->role(member)))) + "}"};
}

Task<Outcome> Service::set_role(std::string id, Role role, std::string by)
{
    TxPtr tx;
    try {
        tx = co_await db_->newTransactionCoro();
        Result r = co_await tx->execSqlCoro("UPDATE members SET role = ? WHERE id = ? AND left_ms IS NULL", std::string(role_name(role)), id);
        if (r.affectedRows() == 0) {
            tx->rollback();
            co_return co_await is_member(id) ? "" : "no member " + id;
        }
        co_await tx->execSqlCoro("DELETE FROM role_offers WHERE member = ?", id);
        int64_t ev = co_await add_event(tx, "member", co_await member_json_of(tx, id));
        if (!co_await Committed(std::move(tx)))
            co_return "the database did not commit it";
        bump(ev);
    } catch (const DrogonDbException &e) {
        if (tx)
            tx->rollback();
        co_return std::string("the database refused it: ") + e.base().what();
    }
    co_await log_admin(by, "set_role", id, role_name(role));
    co_return "";
}

/* ---- bans ---- */

Task<Outcome> Service::ban(std::string id, std::string reason, std::string by)
{
    std::string why;
    if (!clean_text(reason, BAN_REASON_MAX, false, why))
        co_return "a reason is at most " + std::to_string(BAN_REASON_MAX) + " characters";
    try {
        Result known = co_await db_->execSqlCoro("SELECT 1 FROM members WHERE id = ?", id);
        if (known.empty())
            co_return "no member " + id + " has ever joined";
        co_await db_->execSqlCoro("INSERT INTO bans (member, by_who, reason, ts_ms) VALUES (?, ?, ?, ?)"
                                  " ON DUPLICATE KEY UPDATE by_who = VALUES(by_who), reason = VALUES(reason), ts_ms = VALUES(ts_ms)",
                                  id, by, why, now_ms());
    } catch (const DrogonDbException &e) {
        co_return std::string("the database refused it: ") + e.base().what();
    }
    Outcome o = co_await remove_member(id);
    if (o.empty())
        co_await log_admin(by, "ban", id, why);
    co_return o;
}

Task<Outcome> Service::unban(std::string id, std::string by)
{
    try {
        Result r = co_await db_->execSqlCoro("DELETE FROM bans WHERE member = ?", id);
        if (r.affectedRows() == 0)
            co_return id + " is not banned";
    } catch (const DrogonDbException &e) {
        co_return std::string("the database refused it: ") + e.base().what();
    }
    co_await log_admin(by, "unban", id, "");
    co_return "";
}

/* ---- from the app ---- */

Task<std::string> Service::may_act(const std::string &by, const std::string &target)
{
    if (by == target)
        co_return "not_yourself";
    Role b = co_await role(by), t = co_await role(target);
    /* nobody acts on an operator from the app, and only on a lower rank */
    if (t == Role::OPERATOR || b <= t)
        co_return "outranked";
    co_return "";
}

Task<Answer> Service::admin_pond(std::string by, std::optional<std::string> name, std::optional<std::string> description)
{
    if (!name && !description)
        co_return fail(400, "bad_request");
    try {
        std::string n = name ? *name : co_await setting("name");
        Outcome o = co_await set_name(n, description);
        if (o.empty())
            co_await log_admin(by, "set_name", "", n);
        co_return outcome(o);
    } catch (const DrogonDbException &e) {
        co_return server_error(e);
    }
}

Task<Answer> Service::admin_channel_create(std::string by, std::string name, std::string topic)
{
    Outcome o = co_await channel_create(name, topic);
    if (o.empty())
        co_await log_admin(by, "channel_create", name, topic);
    co_return outcome(o);
}

/* The name of live channel `id`, or "" */
static Task<std::string> channel_name_of(const DbPtr &db, int64_t id)
{
    Result r = co_await db->execSqlCoro("SELECT name FROM channels WHERE id = ? AND deleted = 0", id);
    co_return r.empty() ? "" : text(r[0][0]);
}

Task<Answer> Service::admin_channel_edit(std::string by, int64_t channel, std::optional<std::string> name, std::optional<std::string> topic)
{
    if (!name && !topic)
        co_return fail(400, "bad_request");
    try {
        std::string was = co_await channel_name_of(db_, channel);
        if (was.empty())
            co_return fail(404, "no_channel");
        if (topic) {
            Outcome o = co_await channel_topic(was, *topic);
            if (!o.empty())
                co_return outcome(o);
        }
        if (name && *name != was) {
            Outcome o = co_await channel_rename(was, *name);
            if (!o.empty())
                co_return outcome(o);
        }
        co_await log_admin(by, "channel_edit", was, (name ? "name " + *name : "") + (name && topic ? ", " : "") + (topic ? "topic" : ""));
        co_return Answer{204, ""};
    } catch (const DrogonDbException &e) {
        co_return server_error(e);
    }
}

Task<Answer> Service::admin_channel_delete(std::string by, int64_t channel)
{
    try {
        std::string was = co_await channel_name_of(db_, channel);
        if (was.empty())
            co_return fail(404, "no_channel");
        Outcome o = co_await channel_delete(was);
        if (o.empty())
            co_await log_admin(by, "channel_delete", was, "");
        co_return outcome(o);
    } catch (const DrogonDbException &e) {
        co_return server_error(e);
    }
}

Task<Answer> Service::admin_remove(std::string by, std::string member, bool banning, std::string reason)
{
    try {
        std::string no = co_await may_act(by, member);
        if (!no.empty())
            co_return fail(409, no.c_str());
        if (banning)
            co_return outcome(co_await ban(member, reason, by));
        if (!co_await is_member(member))
            co_return fail(404, "no_member");
        Outcome o = co_await remove_member(member);
        if (o.empty())
            co_await log_admin(by, "remove", member, "");
        co_return outcome(o);
    } catch (const DrogonDbException &e) {
        co_return server_error(e);
    }
}

Task<Answer> Service::admin_bans()
{
    try {
        Result r = co_await db_->execSqlCoro("SELECT b.member, b.by_who, b.reason, b.ts_ms, m.profile FROM bans b"
                                             " LEFT JOIN members m ON m.id = b.member ORDER BY b.ts_ms DESC");
        std::string out = "{\"bans\":[";
        for (size_t i = 0; i < r.size(); i++)
            out += std::string(i ? "," : "") + "{\"member\":" + jstr(text(r[i][0])) + ",\"by\":" + jstr(text(r[i][1])) +
                   ",\"reason\":" + jstr(text(r[i][2])) + ",\"ts\":" + std::to_string(num(r[i][3])) +
                   ",\"profile\":" + (r[i][4].isNull() ? std::string("null") : text(r[i][4])) + "}";
        co_return Answer{200, out + "]}"};
    } catch (const DrogonDbException &e) {
        co_return server_error(e);
    }
}

Task<Answer> Service::admin_unban(std::string by, std::string member)
{
    Outcome o = co_await unban(member, by);
    co_return o.empty() ? Answer{204, ""} : fail(404, "no_ban");
}

} // namespace pond
