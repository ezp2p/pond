/* What members may do where (docs/design.md, "Permissions"), as Discord works it out:
 *
 *   1. an operator or administrator may do everything, everywhere;
 *   2. otherwise a member starts from what everyone may do (the Pond's `everyone`) and adds what each role they hold lets them;
 *   3. in a channel, what the channel denies everyone is taken away and what it allows everyone added; then what it denies any
 *      of their roles is taken away, and what it allows any of them added (an allow wins over a deny between roles).
 *
 * A member who may not `view_channels` in a channel does not see it at all. */

#include "pond.h"

#include "json.h"
#include "text.h"

#include <drogon/orm/Exception.h>
#include <trantor/utils/Logger.h>

#include <algorithm>
#include <map>

namespace pond {

using drogon::orm::DrogonDbException;
using drogon::orm::Result;

static Answer server_error(const DrogonDbException &e)
{
    LOG_ERROR << "database: " << e.base().what();
    return fail(500, "database");
}

int64_t permission_bit(const std::string &name)
{
    for (const auto &p : PERMISSIONS)
        if (name == p.name)
            return p.bit;
    return 0;
}

Task<int64_t> Service::everyone_permissions()
{
    const std::string v = co_await setting("everyone");
    co_return v.empty() ? EVERYONE_DEFAULT : (strtoll(v.c_str(), nullptr, 10) & ALL_PERMISSIONS);
}

/* What a member is, for working out what they may do in each channel */
struct Holder {
    bool ranked = false;
    int64_t base = 0;            /* everyone's, and their roles' */
    std::set<int64_t> roles;
};

static int64_t in_channel(const Holder &h, const std::vector<Override> &overrides)
{
    if (h.ranked)
        return ALL_PERMISSIONS;
    int64_t p = h.base, allow = 0, deny = 0;
    for (const Override &o : overrides)
        if (o.role == 0)
            p = (p & ~o.deny) | o.allow;
    for (const Override &o : overrides)
        if (o.role != 0 && h.roles.count(o.role))
            allow |= o.allow, deny |= o.deny;
    return (p & ~deny) | allow;
}

Task<std::map<int64_t, int64_t>> Service::channel_permissions(std::string member)
{
    Holder h;
    const Role rank = co_await role(member);
    h.ranked = rank != Role::MEMBER;
    h.base = co_await everyone_permissions();
    Result r = co_await db_->execSqlCoro("SELECT r.id, r.permissions FROM member_roles mr JOIN roles r ON r.id = mr.role WHERE mr.member = ?", member);
    for (const auto &row : r) {
        h.roles.insert(num(row[0]));
        h.base |= num(row[1]);
    }
    Result c = co_await db_->execSqlCoro("SELECT c.id, o.role, o.allow, o.deny FROM channels c LEFT JOIN channel_overrides o ON o.channel = c.id"
                                         " WHERE c.deleted = 0 ORDER BY c.id");
    std::map<int64_t, std::vector<Override>> overrides;
    for (const auto &row : c) {
        auto &list = overrides[num(row[0])];
        if (!row[1].isNull())
            list.push_back({num(row[1]), num(row[2]), num(row[3])});
    }
    std::map<int64_t, int64_t> out;
    for (const auto &[channel, list] : overrides)
        out[channel] = in_channel(h, list);
    co_return out;
}

Task<int64_t> Service::permissions_in(std::string member, int64_t channel)
{
    const std::map<int64_t, int64_t> all = co_await channel_permissions(member);
    auto it = all.find(channel);
    co_return it == all.end() ? 0 : it->second;
}

Task<std::set<int64_t>> Service::hidden_channels(std::string member)
{
    const std::map<int64_t, int64_t> all = co_await channel_permissions(member);
    std::set<int64_t> hidden;
    for (const auto &[channel, p] : all)
        if (!(p & VIEW_CHANNELS))
            hidden.insert(channel);
    co_return hidden;
}

Task<bool> Service::may_see(std::string member, int64_t channel)
{
    const int64_t p = co_await permissions_in(member, channel);
    co_return (p & VIEW_CHANNELS) != 0;
}

/* What everyone may do: those who may manage roles set it, adding only what they may do themselves. */
Task<Answer> Service::admin_everyone(std::string by, int64_t permissions)
{
    permissions &= ALL_PERMISSIONS;
    const Powers p = co_await powers(by);
    const int64_t had = co_await everyone_permissions();
    if (!p.ranked() && (permissions & ~had & ~p.permissions) != 0)
        co_return fail(409, "outranked");
    TxPtr tx;
    try {
        tx = co_await db_->newTransactionCoro();
        co_await tx->execSqlCoro("REPLACE INTO settings (name, value) VALUES ('everyone', ?)", std::to_string(permissions));
        const std::string pj = co_await pond_json(tx);
        co_await add_event(tx, "pond", pj);
        const int64_t ev = co_await add_event(tx, "channels", "{}"); /* who sees what may have changed */
        if (!co_await Committed(std::move(tx)))
            co_return fail(500, "database");
        bump(ev);
    } catch (const DrogonDbException &e) {
        if (tx)
            tx->rollback();
        co_return server_error(e);
    }
    co_await log_admin(by, "everyone", "", permissions_json(permissions));
    co_return Answer{204, ""};
}

/* What a channel allows and denies each role, all at once. Someone without a rank may only touch everyone's and the roles
 * below their own highest, and only allow what they may do themselves. */
Task<Answer> Service::admin_channel_overrides(std::string by, int64_t channel, std::vector<Override> list)
{
    std::map<int64_t, Override> want;
    for (Override o : list) {
        if (o.role < 0 || ((o.allow | o.deny) & ~CHANNEL_PERMISSIONS) || (o.allow & o.deny) || want.count(o.role))
            co_return fail(400, "bad_override");
        if (o.allow || o.deny)
            want[o.role] = o;
    }
    const Powers p = co_await powers(by);
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
        Result had = co_await tx->execSqlCoro("SELECT role, allow, deny FROM channel_overrides WHERE channel = ?", channel);
        std::map<int64_t, Override> before;
        for (const auto &row : had)
            before[num(row[0])] = {num(row[0]), num(row[1]), num(row[2])};
        /* every role named exists; those changed are theirs to change */
        std::set<int64_t> touched;
        for (const auto &[r, o] : want)
            touched.insert(r);
        for (const auto &[r, o] : before)
            touched.insert(r);
        for (int64_t r : touched) {
            const Override a = want.count(r) ? want[r] : Override{r, 0, 0}, b = before.count(r) ? before[r] : Override{r, 0, 0};
            if (a.allow == b.allow && a.deny == b.deny)
                continue;
            if (r != 0) {
                Result q = co_await tx->execSqlCoro("SELECT position FROM roles WHERE id = ?", r);
                if (q.empty() && want.count(r)) {
                    tx->rollback();
                    co_return fail(404, "unknown_role");
                }
                if (!p.ranked() && !q.empty() && num(q[0][0]) <= p.top) {
                    tx->rollback();
                    co_return fail(409, "outranked");
                }
            }
            if (!p.ranked() && (a.allow & ~b.allow & ~p.permissions) != 0) {
                tx->rollback();
                co_return fail(409, "outranked");
            }
        }
        co_await tx->execSqlCoro("DELETE FROM channel_overrides WHERE channel = ?", channel);
        for (const auto &[r, o] : want)
            co_await tx->execSqlCoro("INSERT INTO channel_overrides (channel, role, allow, deny) VALUES (?, ?, ?, ?)", channel, r, o.allow, o.deny);
        const int64_t ev = co_await add_event(tx, "channels", "{}");
        if (!co_await Committed(std::move(tx)))
            co_return fail(500, "database");
        bump(ev);
    } catch (const DrogonDbException &e) {
        if (tx)
            tx->rollback();
        co_return server_error(e);
    }
    std::vector<Override> kept;
    for (const auto &[r, o] : want)
        kept.push_back(o);
    co_await log_admin(by, "channel_overrides", name, overrides_json(kept));
    co_return Answer{204, ""};
}

/* Who may see a channel, as protocol 9 said it: everyone (nullopt), or only operators, administrators and these roles. It is
 * kept as overrides: everyone is denied the view and these roles allowed it; the rest of what the channel says stays. */
Task<Answer> Service::admin_channel_roles(std::string by, int64_t channel, std::optional<std::vector<int64_t>> roles)
{
    std::vector<int64_t> want = roles.value_or(std::vector<int64_t>{});
    std::sort(want.begin(), want.end());
    want.erase(std::unique(want.begin(), want.end()), want.end());
    if ((int64_t)want.size() > ROLES_MAX)
        co_return fail(400, "bad_request");
    std::vector<Override> list;
    try {
        Result had = co_await db_->execSqlCoro("SELECT role, allow, deny FROM channel_overrides WHERE channel = ?", channel);
        std::map<int64_t, Override> now;
        for (const auto &row : had)
            now[num(row[0])] = {num(row[0]), num(row[1]), num(row[2])};
        for (auto &[r, o] : now)
            o.allow &= ~VIEW_CHANNELS, o.deny &= ~VIEW_CHANNELS;
        if (roles) {
            now[0].role = 0, now[0].deny |= VIEW_CHANNELS;
            for (int64_t r : want)
                now[r].role = r, now[r].allow |= VIEW_CHANNELS, now[r].deny &= ~VIEW_CHANNELS;
        }
        for (const auto &[r, o] : now)
            if (o.allow || o.deny)
                list.push_back(o);
    } catch (const DrogonDbException &e) {
        co_return server_error(e);
    }
    co_return co_await admin_channel_overrides(by, channel, list);
}

} // namespace pond
