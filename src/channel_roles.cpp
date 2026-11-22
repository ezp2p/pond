/* Roles (docs/design.md, "Roles for channels" and "Permissions"): named groups the operators and administrators make and give
 * to members, so a channel can be for some members only, and so members can be let do some of what an administrator does; and
 * the order of the channels, which the admins set. A channel that is restricted is seen by operators, administrators, and
 * members holding one of its roles; one that is not, by everyone.
 *
 * A member who is let manage roles (not an operator or administrator) works below their own highest role, as on Discord: they
 * may only change, delete, give or take roles lower than it, move only those, and give a role only permissions they have. */

#include "pond.h"

#include "json.h"
#include "text.h"

#include <drogon/orm/Exception.h>
#include <trantor/utils/Logger.h>

#include <algorithm>
#include <iterator>

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

std::string permissions_json(int64_t bits)
{
    std::string out = "[";
    for (const auto &p : PERMISSIONS)
        if (bits & p.bit)
            out += std::string(out.size() > 1 ? "," : "") + "\"" + p.name + "\"";
    return out + "]";
}

std::string role_json(int64_t id, const std::string &name, const std::string &color, bool hoist, int64_t permissions)
{
    return "{\"id\":" + std::to_string(id) + ",\"name\":" + jstr(name) + ",\"color\":" + (color.empty() ? std::string("null") : jstr(color)) +
           ",\"hoist\":" + (hoist ? "true" : "false") + ",\"permissions\":" + permissions_json(permissions) + "}";
}

/* "#rrggbb", lower case; or "" for none */
static bool color_ok(const std::string &c)
{
    if (c.empty())
        return true;
    if (c.size() != 7 || c[0] != '#')
        return false;
    for (size_t i = 1; i < 7; i++)
        if (!((c[i] >= '0' && c[i] <= '9') || (c[i] >= 'a' && c[i] <= 'f')))
            return false;
    return true;
}

static const char OUTRANKED[] = "outranked";

Task<Powers> Service::powers(std::string member)
{
    Powers p;
    p.rank = co_await role(member);
    Result r = co_await db_->execSqlCoro("SELECT COALESCE(BIT_OR(r.permissions), 0), MIN(r.position) FROM member_roles mr"
                                         " JOIN roles r ON r.id = mr.role WHERE mr.member = ?",
                                         member);
    if (!r.empty()) {
        p.permissions = num(r[0][0]);
        if (!r[0][1].isNull())
            p.top = num(r[0][1]);
    }
    co_return p;
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

static const char NAME_RULE[] = "a role's name is 1 to 32 characters on one line";

Task<Answer> Service::admin_role_create(std::string by, RoleEdit edit)
{
    std::string n;
    if (!edit.name || !role_name_ok(*edit.name, n))
        co_return bad(NAME_RULE);
    const std::string color = edit.color.value_or("");
    if (!color_ok(color))
        co_return bad("a role's colour is #rrggbb");
    const int64_t perms = edit.permissions.value_or(0);
    const bool hoist = edit.hoist.value_or(false);
    const Powers p = co_await powers(by);
    if (!p.ranked() && (perms & ~p.permissions) != 0) /* only what they may do themselves */
        co_return fail(409, OUTRANKED);
    TxPtr tx;
    int64_t id = 0;
    try {
        tx = co_await db_->newTransactionCoro();
        Result c = co_await tx->execSqlCoro("SELECT COUNT(*), COALESCE(MAX(position) + 1, 0) FROM roles FOR UPDATE");
        if (num(c[0][0]) >= ROLES_MAX) {
            tx->rollback();
            co_return bad("a Pond has at most " + std::to_string(ROLES_MAX) + " roles");
        }
        const int64_t position = num(c[0][1]); /* the lowest */
        Result r = co_await tx->execSqlCoro("INSERT INTO roles (name, created_ms, color, hoist, permissions, position) VALUES (?, ?, ?, ?, ?, ?)",
                                            n, now_ms(), color, (int64_t)(hoist ? 1 : 0), perms, position);
        id = (int64_t)r.insertId();
        int64_t ev = co_await add_event(tx, "role", role_json(id, n, color, hoist, perms));
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
    co_await log_admin(by, "role_create", n, permissions_json(perms));
    co_return Answer{200, role_json(id, n, color, hoist, perms)};
}

Task<Answer> Service::admin_role_edit(std::string by, int64_t role, RoleEdit edit)
{
    if (!edit.name && !edit.color && !edit.hoist && !edit.permissions)
        co_return fail(400, "bad_request");
    std::string n;
    if (edit.name && !role_name_ok(*edit.name, n))
        co_return bad(NAME_RULE);
    if (edit.color && !color_ok(*edit.color))
        co_return bad("a role's colour is #rrggbb");
    const Powers p = co_await powers(by);
    TxPtr tx;
    std::string was, detail;
    try {
        tx = co_await db_->newTransactionCoro();
        Result r = co_await tx->execSqlCoro("SELECT name, color, hoist, permissions, position FROM roles WHERE id = ? FOR UPDATE", role);
        if (r.empty()) {
            tx->rollback();
            co_return fail(404, "unknown_role");
        }
        was = text(r[0][0]);
        const std::string name = edit.name ? n : was;
        const std::string color = edit.color ? *edit.color : text(r[0][1]);
        const bool hoist = edit.hoist ? *edit.hoist : num(r[0][2]) != 0;
        const int64_t had = num(r[0][3]), perms = edit.permissions ? *edit.permissions : had;
        /* below their highest role, and adding only what they may do themselves */
        if (!p.ranked() && (num(r[0][4]) <= p.top || (perms & ~had & ~p.permissions) != 0)) {
            tx->rollback();
            co_return fail(409, OUTRANKED);
        }
        co_await tx->execSqlCoro("UPDATE roles SET name = ?, color = ?, hoist = ?, permissions = ? WHERE id = ?", name, color,
                                 (int64_t)(hoist ? 1 : 0), perms, role);
        int64_t ev = co_await add_event(tx, "role", role_json(role, name, color, hoist, perms));
        if (!co_await Committed(std::move(tx)))
            co_return fail(500, "database");
        bump(ev);
        if (edit.name)
            detail = "name " + name;
        if (edit.permissions)
            detail += (detail.empty() ? "" : ", ") + std::string("permissions ") + permissions_json(perms);
        if (edit.color || edit.hoist)
            detail += (detail.empty() ? "" : ", ") + std::string("looks");
    } catch (const DrogonDbException &e) {
        if (tx)
            tx->rollback();
        if (sql_errno(e) == 1062)
            co_return bad("there is already a role called " + n);
        co_return server_error(e);
    }
    co_await log_admin(by, "role_edit", was, detail);
    co_return Answer{204, ""};
}

/* A channel that asked for this role alone stays restricted, so it is left to operators and administrators rather than opened
 * to everyone. Members' Puddles take the role out of each member and channel themselves, on `role_deleted`. */
Task<Answer> Service::admin_role_delete(std::string by, int64_t role)
{
    const Powers p = co_await powers(by);
    TxPtr tx;
    std::string was;
    try {
        tx = co_await db_->newTransactionCoro();
        Result r = co_await tx->execSqlCoro("SELECT name, position FROM roles WHERE id = ? FOR UPDATE", role);
        if (r.empty()) {
            tx->rollback();
            co_return fail(404, "unknown_role");
        }
        if (!p.ranked() && num(r[0][1]) <= p.top) {
            tx->rollback();
            co_return fail(409, OUTRANKED);
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
    const Powers p = co_await powers(by);
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
        if (!p.ranked()) { /* each role given or taken is below their highest */
            Result h = co_await tx->execSqlCoro("SELECT role FROM member_roles WHERE member = ?", member);
            std::vector<int64_t> had, changed;
            for (const auto &row : h)
                had.push_back(num(row[0]));
            had = tidy(std::move(had));
            std::set_symmetric_difference(had.begin(), had.end(), roles.begin(), roles.end(), std::back_inserter(changed));
            if (!changed.empty()) {
                Result t = co_await tx->execSqlCoro("SELECT MIN(position) FROM roles WHERE id IN (" + ids_text(changed) + ")");
                if (!t.empty() && !t[0][0].isNull() && num(t[0][0]) <= p.top) {
                    tx->rollback();
                    co_return fail(409, OUTRANKED);
                }
            }
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

Task<Answer> Service::admin_role_order(std::string by, std::vector<int64_t> roles)
{
    const Powers p = co_await powers(by);
    TxPtr tx;
    try {
        tx = co_await db_->newTransactionCoro();
        Result r = co_await tx->execSqlCoro("SELECT id, position FROM roles ORDER BY position, id FOR UPDATE");
        std::vector<int64_t> now, live, asked = roles;
        size_t fixed = 0; /* the roles at or above their highest, which stay where they are */
        for (const auto &row : r) {
            now.push_back(num(row[0]));
            if (!p.ranked() && num(row[1]) <= p.top)
                fixed = now.size();
        }
        live = now;
        std::sort(live.begin(), live.end());
        std::sort(asked.begin(), asked.end());
        if (live != asked) { /* each role once: one made or deleted meanwhile is a conflict, to ask again */
            tx->rollback();
            co_return fail(409, "roles_changed");
        }
        if (!std::equal(now.begin(), now.begin() + (std::ptrdiff_t)fixed, roles.begin())) {
            tx->rollback();
            co_return fail(409, OUTRANKED);
        }
        for (size_t i = 0; i < roles.size(); i++)
            co_await tx->execSqlCoro("UPDATE roles SET position = ? WHERE id = ?", (int64_t)i, roles[i]);
        int64_t ev = co_await add_event(tx, "role_order", "{\"roles\":[" + ids_text(roles) + "]}");
        if (!co_await Committed(std::move(tx)))
            co_return fail(500, "database");
        bump(ev);
    } catch (const DrogonDbException &e) {
        if (tx)
            tx->rollback();
        co_return server_error(e);
    }
    co_await log_admin(by, "role_order", "", ids_text(roles));
    co_return Answer{204, ""};
}


/* ------------------------------------------------------------------ role colours (docs/design.md, "Role colours") */

/* A member chooses whether their name shows in their highest coloured role's colour, rather than their profile's. */
Task<Answer> Service::role_color(std::string member, bool use)
{
    TxPtr tx;
    try {
        tx = co_await db_->newTransactionCoro();
        const int value = use ? 1 : 0;
        Result r = co_await tx->execSqlCoro("UPDATE members SET role_color = ? WHERE id = ? AND left_ms IS NULL", value, member);
        int64_t ev = 0;
        if (r.affectedRows()) { /* everyone's app shows their name in the new colour */
            const std::string mj = co_await member_json_of(tx, member);
            ev = co_await add_event(tx, "member", mj);
        }
        if (!co_await Committed(std::move(tx)))
            co_return fail(500, "database");
        bump(ev);
        const std::string said = use ? "true" : "false";
        co_return Answer{200, "{\"use\":" + said + "}"};
    } catch (const DrogonDbException &e) {
        if (tx)
            tx->rollback();
        LOG_ERROR << "database: " << e.base().what();
        co_return fail(500, "database");
    }
}

/* The Pond makes everyone's name show in their role's colour (`enforced`), or leaves it to each member. */
Task<Answer> Service::admin_role_colors(std::string by, bool enforced)
{
    TxPtr tx;
    try {
        tx = co_await db_->newTransactionCoro();
        if (enforced)
            co_await tx->execSqlCoro("REPLACE INTO settings (name, value) VALUES ('role_colors', 'enforced')");
        else
            co_await tx->execSqlCoro("DELETE FROM settings WHERE name = 'role_colors'");
        const std::string pj = co_await pond_json(tx);
        const int64_t ev = co_await add_event(tx, "pond", pj);
        if (!co_await Committed(std::move(tx)))
            co_return fail(500, "database");
        bump(ev);
    } catch (const DrogonDbException &e) {
        if (tx)
            tx->rollback();
        LOG_ERROR << "database: " << e.base().what();
        co_return fail(500, "database");
    }
    const std::string detail = enforced ? "enforced" : "choice";
    co_await log_admin(by, "set_role_colors", "", detail);
    co_return Answer{204, ""};
}

} // namespace pond
