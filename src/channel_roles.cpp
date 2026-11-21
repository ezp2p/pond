/* Roles for channels (docs/design.md, "Roles for channels"): named groups the operators and administrators make and give to
 * members, so a channel can be for some members only; and the order of the channels, which the admins set. A channel that is
 * restricted is seen by operators, administrators, and members holding one of its roles; one that is not, by everyone. */

#include "pond.h"

#include "json.h"
#include "text.h"

#include <drogon/orm/Exception.h>
#include <trantor/utils/Logger.h>

#include <algorithm>

namespace pond {

using drogon::orm::DrogonDbException;
using drogon::orm::Result;

static Answer server_error(const DrogonDbException &e)
{
    LOG_ERROR << "database: " << e.base().what();
    return fail(500, "database");
}

static Answer bad(const std::string &message)
{
    return {400, "{\"error\":\"bad_request\",\"message\":" + jstr(message) + "}"};
}

/* A role's name: one line of 1 to ROLE_NAME_MAX characters, cleaned. false if it can't be. */
static bool role_name_ok(const std::string &in, std::string &out)
{
    return clean_text(in, ROLE_NAME_MAX, false, out) && !out.empty();
}

static std::string role_json(int64_t id, const std::string &name)
{
    return "{\"id\":" + std::to_string(id) + ",\"name\":" + jstr(name) + "}";
}

static std::string ids_text(const std::vector<int64_t> &ids)
{
    std::string out;
    for (int64_t id : ids)
        out += (out.empty() ? "" : ",") + std::to_string(id);
    return out;
}

/* `ids` sorted with no repeats, and every one a role that exists: false if one does not */
static Task<bool> roles_exist(TxPtr tx, std::vector<int64_t> ids)
{
    if (ids.empty())
        co_return true;
    /* numbers, so safe to put in the statement */
    Result r = co_await tx->execSqlCoro("SELECT COUNT(*) FROM roles WHERE id IN (" + ids_text(ids) + ") FOR UPDATE");
    co_return num(r[0][0]) == (int64_t)ids.size();
}

static std::vector<int64_t> tidy(std::vector<int64_t> ids)
{
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

Task<Answer> Service::admin_role_create(std::string by, std::string name)
{
    std::string n;
    if (!role_name_ok(name, n))
        co_return bad("a role's name is 1 to " + std::to_string(ROLE_NAME_MAX) + " characters on one line");
    TxPtr tx;
    int64_t id = 0;
    try {
        tx = co_await db_->newTransactionCoro();
        Result c = co_await tx->execSqlCoro("SELECT COUNT(*) FROM roles");
        if (num(c[0][0]) >= ROLES_MAX) {
            tx->rollback();
            co_return bad("a Pond has at most " + std::to_string(ROLES_MAX) + " roles");
        }
        Result r = co_await tx->execSqlCoro("INSERT INTO roles (name, created_ms) VALUES (?, ?)", n, now_ms());
        id = (int64_t)r.insertId();
        int64_t ev = co_await add_event(tx, "role", role_json(id, n));
        if (!co_await Committed(std::move(tx)))
            co_return fail(500, "database");
        bump(ev);
    } catch (const DrogonDbException &e) {
        if (tx)
            tx->rollback();
        if (sql_errno(e) == 1062)
            co_return bad("there is already a role called " + n);
        co_return server_error(e);
    }
    co_await log_admin(by, "role_create", n, "");
    co_return Answer{200, role_json(id, n)};
}

Task<Answer> Service::admin_role_edit(std::string by, int64_t role, std::string name)
{
    std::string n;
    if (!role_name_ok(name, n))
        co_return bad("a role's name is 1 to " + std::to_string(ROLE_NAME_MAX) + " characters on one line");
    TxPtr tx;
    std::string was;
    try {
        tx = co_await db_->newTransactionCoro();
        Result r = co_await tx->execSqlCoro("SELECT name FROM roles WHERE id = ? FOR UPDATE", role);
        if (r.empty()) {
            tx->rollback();
            co_return fail(404, "unknown_role");
        }
        was = text(r[0][0]);
        co_await tx->execSqlCoro("UPDATE roles SET name = ? WHERE id = ?", n, role);
        int64_t ev = co_await add_event(tx, "role", role_json(role, n));
        if (!co_await Committed(std::move(tx)))
            co_return fail(500, "database");
        bump(ev);
    } catch (const DrogonDbException &e) {
        if (tx)
            tx->rollback();
        if (sql_errno(e) == 1062)
            co_return bad("there is already a role called " + n);
        co_return server_error(e);
    }
    co_await log_admin(by, "role_edit", was, "name " + n);
    co_return Answer{204, ""};
}

/* A channel that asked for this role alone stays restricted, so it is left to operators and administrators rather than opened
 * to everyone. Members' Puddles take the role out of each member and channel themselves, on `role_deleted`. */
Task<Answer> Service::admin_role_delete(std::string by, int64_t role)
{
    TxPtr tx;
    std::string was;
    try {
        tx = co_await db_->newTransactionCoro();
        Result r = co_await tx->execSqlCoro("SELECT name FROM roles WHERE id = ? FOR UPDATE", role);
        if (r.empty()) {
            tx->rollback();
            co_return fail(404, "unknown_role");
        }
        was = text(r[0][0]);
        co_await tx->execSqlCoro("DELETE FROM member_roles WHERE role = ?", role);
        co_await tx->execSqlCoro("DELETE FROM channel_roles WHERE role = ?", role);
        co_await tx->execSqlCoro("DELETE FROM roles WHERE id = ?", role);
        co_await add_event(tx, "role_deleted", "{\"id\":" + std::to_string(role) + "}");
        int64_t ev = co_await add_event(tx, "channels", "{}"); /* who sees what may have changed */
        if (!co_await Committed(std::move(tx)))
            co_return fail(500, "database");
        bump(ev);
    } catch (const DrogonDbException &e) {
        if (tx)
            tx->rollback();
        co_return server_error(e);
    }
    co_await log_admin(by, "role_delete", was, "");
    co_return Answer{204, ""};
}

Task<Answer> Service::admin_member_roles(std::string by, std::string member, std::vector<int64_t> roles)
{
    roles = tidy(std::move(roles));
    if ((int64_t)roles.size() > ROLES_MAX)
        co_return fail(400, "bad_request");
    TxPtr tx;
    try {
        tx = co_await db_->newTransactionCoro();
        Result m = co_await tx->execSqlCoro("SELECT 1 FROM members WHERE id = ? AND left_ms IS NULL FOR UPDATE", member);
        if (m.empty()) {
            tx->rollback();
            co_return fail(404, "no_member");
        }
        const bool known = co_await roles_exist(tx, roles);
        if (!known) {
            tx->rollback();
            co_return fail(404, "unknown_role");
        }
        co_await tx->execSqlCoro("DELETE FROM member_roles WHERE member = ?", member);
        for (int64_t r : roles)
            co_await tx->execSqlCoro("INSERT INTO member_roles (member, role) VALUES (?, ?)", member, r);
        std::string mj = co_await member_json_of(tx, member);
        co_await add_event(tx, "member", mj);
        int64_t ev = co_await add_event(tx, "channels", "{}", member); /* what they see now */
        if (!co_await Committed(std::move(tx)))
            co_return fail(500, "database");
        bump(ev);
    } catch (const DrogonDbException &e) {
        if (tx)
            tx->rollback();
        co_return server_error(e);
    }
    co_await log_admin(by, "member_roles", member, ids_text(roles));
    co_return Answer{204, ""};
}

Task<Answer> Service::admin_channel_roles(std::string by, int64_t channel, std::optional<std::vector<int64_t>> roles)
{
    if (roles)
        roles = tidy(std::move(*roles));
    if (roles && (int64_t)roles->size() > ROLES_MAX)
        co_return fail(400, "bad_request");
    TxPtr tx;
    std::string name;
    try {
        tx = co_await db_->newTransactionCoro();
        Result c = co_await tx->execSqlCoro("SELECT name FROM channels WHERE id = ? AND deleted = 0 FOR UPDATE", channel);
        if (c.empty()) {
            tx->rollback();
            co_return fail(404, "no_channel");
        }
        name = text(c[0][0]);
        const std::vector<int64_t> want = roles.value_or(std::vector<int64_t>{});
        const bool known = co_await roles_exist(tx, want);
        if (!known) {
            tx->rollback();
            co_return fail(404, "unknown_role");
        }
        const int64_t restricted = roles ? 1 : 0;
        co_await tx->execSqlCoro("UPDATE channels SET restricted = ? WHERE id = ?", restricted, channel);
        co_await tx->execSqlCoro("DELETE FROM channel_roles WHERE channel = ?", channel);
        for (int64_t r : want)
            co_await tx->execSqlCoro("INSERT INTO channel_roles (channel, role) VALUES (?, ?)", channel, r);
        int64_t ev = co_await add_event(tx, "channels", "{}");
        if (!co_await Committed(std::move(tx)))
            co_return fail(500, "database");
        bump(ev);
    } catch (const DrogonDbException &e) {
        if (tx)
            tx->rollback();
        co_return server_error(e);
    }
    std::string detail = "everyone";
    if (roles)
        detail = roles->empty() ? std::string("operators and administrators") : "roles " + ids_text(*roles);
    co_await log_admin(by, "channel_roles", name, detail);
    co_return Answer{204, ""};
}

Task<Answer> Service::admin_channel_order(std::string by, std::vector<int64_t> channels)
{
    TxPtr tx;
    try {
        tx = co_await db_->newTransactionCoro();
        Result r = co_await tx->execSqlCoro("SELECT id FROM channels WHERE deleted = 0 FOR UPDATE");
        std::vector<int64_t> live, asked = channels;
        for (const auto &row : r)
            live.push_back(num(row[0]));
        std::sort(live.begin(), live.end());
        std::sort(asked.begin(), asked.end());
        if (live != asked) { /* each live channel once: one made or deleted meanwhile is a conflict, to ask again */
            tx->rollback();
            co_return fail(409, "channels_changed");
        }
        for (size_t i = 0; i < channels.size(); i++)
            co_await tx->execSqlCoro("UPDATE channels SET position = ? WHERE id = ?", (int64_t)i, channels[i]);
        int64_t ev = co_await add_event(tx, "channels", "{}");
        if (!co_await Committed(std::move(tx)))
            co_return fail(500, "database");
        bump(ev);
    } catch (const DrogonDbException &e) {
        if (tx)
            tx->rollback();
        co_return server_error(e);
    }
    co_await log_admin(by, "channel_order", "", ids_text(channels));
    co_return Answer{204, ""};
}

} // namespace pond
