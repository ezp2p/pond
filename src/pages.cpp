/* Pages (docs/design.md, "Pages"): what a Pond shows its members above the channels, as Discord's server guide is: rules, a list
 * of servers, a guide. A page is a title, a kind, a body of markdown and the media it carries (files sent to the Pond first, as
 * a message's; its markdown shows a picture by naming it, `![](media:name.png)`), in an order the admins set. A page is for
 * everyone, or for some roles only. Those who may manage pages (operators, administrators, members whose roles say
 * `manage_pages`) make, change, order and delete them.
 *
 * Each kind adds to the markdown (its `data`, a JSON object): "roles" offers roles members pick for themselves; "events" is a
 * calendar; "form" is the questions those joining answer (applications.cpp); "shop" sells things (shop.cpp). */

#include "pond.h"

#include "json.h"
#include "text.h"

#include <drogon/orm/Exception.h>
#include <trantor/utils/Logger.h>

#include <algorithm>
#include <set>

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

static std::string ids_text(const std::vector<int64_t> &ids)
{
    std::string out;
    for (int64_t id : ids)
        out += (out.empty() ? "" : ",") + std::to_string(id);
    return out;
}

/* {id, title, kind, updated, roles}: as /info lists a page, and the `page` event says one; `roles` null for everyone */
static std::string page_head(int64_t id, const std::string &title, const std::string &kind, int64_t updated, const std::string &roles = "null")
{
    return "{\"id\":" + std::to_string(id) + ",\"title\":" + jstr(title) + ",\"kind\":" + jstr(kind) + ",\"updated\":" + std::to_string(updated) +
           ",\"roles\":" + roles + "}";
}

static std::string compact(const Json::Value &v)
{
    Json::StreamWriterBuilder b;
    b["indentation"] = "";
    b["emitUTF8"] = true;
    return Json::writeString(b, v);
}

/* A short line of text, cleaned: false if it is not one of 0 (or 1, `needed`) to `max` characters */
static bool line(const Json::Value &v, size_t max, bool needed, bool multiline, std::string &out)
{
    if (!v.isString() || !clean_text(v.asString(), max, multiline, out))
        return false;
    return !needed || !out.empty();
}

/* A kind's settings (`data`), checked and written again in a fixed shape: "" if they may be set, else why not */
static std::string clean_data(const std::string &kind, const std::string &in, std::string &out)
{
    Json::Value v, o(Json::objectValue);
    if (in.size() > PAGE_DATA_MAX || !parse_object(in, v))
        return "a page's settings are a JSON object of at most 64 KiB";
    std::string t;
    if (kind == "roles") {
        /* {roles: [{role, description}], multiple: bool}: the roles members may take for themselves */
        const Json::Value a = v.isMember("roles") ? v["roles"] : Json::Value(Json::arrayValue); /* none yet: a new page */
        if (!a.isArray() || a.size() > PAGE_OPTIONS_MAX)
            return "a roles page offers up to 50 roles";
        o["roles"] = Json::arrayValue;
        std::set<int64_t> seen;
        for (const auto &r : a) {
            Json::Value e;
            if (!r.isObject() || !r["role"].isInt64() || r["role"].asInt64() < 1 || !seen.insert(r["role"].asInt64()).second)
                return "each role a page offers is named once by its id";
            e["role"] = Json::Int64(r["role"].asInt64());
            if (!line(r.get("description", ""), FORM_TEXT_MAX, false, false, t))
                return "a role's description is one line of up to 500 characters";
            e["description"] = t;
            o["roles"].append(e);
        }
        o["multiple"] = v.get("multiple", true).isBool() ? v.get("multiple", true).asBool() : true;
    } else if (kind == "form") {
        /* {questions: [{id, label, kind: text|long|choice|checks, choices: [], required}], required_to_join: bool} */
        const Json::Value a = v.isMember("questions") ? v["questions"] : Json::Value(Json::arrayValue);
        if (!a.isArray() || a.size() > FORM_QUESTIONS_MAX)
            return "a form has up to 30 questions";
        o["questions"] = Json::arrayValue;
        std::set<std::string> ids;
        for (const auto &q : a) {
            Json::Value e;
            std::string id, label, qk;
            if (!q.isObject() || !line(q["id"], 16, true, false, id) || !ids.insert(id).second)
                return "each question has an id of its own, up to 16 characters";
            if (!line(q["label"], FORM_TEXT_MAX, true, false, label))
                return "each question is one line of up to 500 characters";
            qk = q.get("kind", "text").isString() ? q.get("kind", "text").asString() : "";
            if (qk != "text" && qk != "long" && qk != "choice" && qk != "checks")
                return "a question is text, long, choice or checks";
            e["id"] = id, e["label"] = label, e["kind"] = qk;
            e["required"] = q.get("required", false).isBool() && q.get("required", false).asBool();
            e["choices"] = Json::arrayValue;
            if (qk == "choice" || qk == "checks") {
                const Json::Value &c = q["choices"];
                if (!c.isArray() || c.empty() || c.size() > 20)
                    return "a choice question has 1 to 20 choices";
                for (const auto &x : c) {
                    if (!line(x, FORM_TEXT_MAX, true, false, t))
                        return "each choice is one line of up to 500 characters";
                    e["choices"].append(t);
                }
            }
            o["questions"].append(e);
        }
        o["required_to_join"] = v.get("required_to_join", false).isBool() && v.get("required_to_join", false).asBool();
    } else if (kind == "shop") {
        /* {currency: "usd"}: what its prices are in, as Stripe names currencies */
        std::string c = v.get("currency", "usd").isString() ? v.get("currency", "usd").asString() : "";
        if (c.size() != 3 || !std::all_of(c.begin(), c.end(), [](char ch) { return ch >= 'a' && ch <= 'z'; }))
            return "a shop's currency is three lower-case letters, as Stripe names them (usd, eur, ...)";
        o["currency"] = c;
    }
    out = compact(o);
    return "";
}

static bool kind_ok(const std::string &k)
{
    return std::find(PAGE_KINDS.begin(), PAGE_KINDS.end(), k) != PAGE_KINDS.end();
}

/* What `edit` asks, checked and cleaned in place: "" if it may be set, else why not (for the app to show). */
static std::string check(PageEdit &edit)
{
    std::string t, b;
    if (edit.title) {
        if (!clean_text(*edit.title, PAGE_TITLE_MAX, false, t) || t.empty())
            return "a page's title is 1 to " + std::to_string(PAGE_TITLE_MAX) + " characters on one line";
        edit.title = t;
    }
    if (edit.kind && !kind_ok(*edit.kind))
        return "unknown_kind";
    if (edit.body) {
        if (!clean_text(*edit.body, PAGE_BODY_MAX, true, b))
            return "a page is at most " + std::to_string(PAGE_BODY_MAX / 1024) + " KiB of text";
        edit.body = b;
    }
    if (edit.roles && *edit.roles && (*edit.roles)->size() > (size_t)ROLES_MAX)
        return "bad_roles";
    if (edit.files) {
        if (edit.files->size() > PAGE_FILES_MAX)
            return "a page carries at most " + std::to_string(PAGE_FILES_MAX) + " files";
        for (auto &f : *edit.files)
            if (!clean_file(f))
                return "bad_file";
    }
    return "";
}

static Answer refused(const std::string &why)
{
    if (why == "unknown_kind" || why == "bad_file" || why == "bad_roles")
        return fail(400, why.c_str());
    return bad(why);
}

/* The page's files: each must be on the Pond whole (sent first). Replaces what it carried. false (and `missing` set) if one is not. */
static Task<bool> put_files(TxPtr tx, int64_t page, std::vector<FileRef> files, std::string *missing)
{
    std::vector<int64_t> sizes;
    for (auto &f : files) {
        Result w = co_await tx->execSqlCoro("SELECT size FROM files WHERE id = ? AND whole = 1 LOCK IN SHARE MODE", f.id);
        if (w.empty()) {
            *missing = f.id;
            co_return false;
        }
        sizes.push_back(num(w[0][0]));
    }
    co_await tx->execSqlCoro("DELETE FROM page_files WHERE page = ?", page);
    for (size_t i = 0; i < files.size(); i++) {
        const FileRef &f = files[i];
        auto opt = [](int64_t v) { return v > 0 ? std::optional<int64_t>(v) : std::nullopt; };
        co_await tx->execSqlCoro("INSERT INTO page_files (page, position, file, name, mime, size, w, h) VALUES (?, ?, ?, ?, ?, ?, ?, ?)", page,
                                 (int64_t)i, f.id, f.name, f.mime, sizes[i], opt(f.w), opt(f.h));
    }
    co_return true;
}

/* "null", or the roles a page is for, as a JSON array */
static Task<std::string> page_roles_of(DbPtr db, int64_t page, bool restricted)
{
    if (!restricted)
        co_return "null";
    Result r = co_await db->execSqlCoro("SELECT role FROM page_roles WHERE page = ? ORDER BY role", page);
    std::vector<int64_t> ids;
    for (const auto &row : r)
        ids.push_back(num(row[0]));
    co_return "[" + ids_text(ids) + "]";
}

/* Who sees a page for some roles only: operators, administrators, those who may manage pages, and holders of one of them */
static bool sees(const Powers &p, const std::set<int64_t> &held, bool restricted, const std::vector<int64_t> &roles)
{
    if (!restricted || p.has(MANAGE_PAGES))
        return true;
    for (int64_t r : roles)
        if (held.count(r))
            return true;
    return false;
}

static Task<std::set<int64_t>> held_roles(DbPtr db, std::string member)
{
    Result r = co_await db->execSqlCoro("SELECT role FROM member_roles WHERE member = ?", member);
    std::set<int64_t> out;
    for (const auto &row : r)
        out.insert(num(row[0]));
    co_return out;
}

static std::vector<int64_t> id_vector(const std::string &concat)
{
    std::vector<int64_t> out;
    size_t p = 0;
    while (p < concat.size()) {
        size_t e = concat.find(',', p);
        if (e == std::string::npos)
            e = concat.size();
        out.push_back(strtoll(concat.substr(p, e - p).c_str(), nullptr, 10));
        p = e + 1;
    }
    return out;
}

Task<std::string> Service::pages_json()
{
    Result r = co_await db_->execSqlCoro("SELECT id, title, kind, updated_ms FROM pages WHERE restricted = 0 ORDER BY position, id");
    std::string out = "[";
    for (size_t i = 0; i < r.size(); i++)
        out += std::string(i ? "," : "") + page_head(num(r[i][0]), text(r[i][1]), text(r[i][2]), num(r[i][3]));
    co_return out + "]";
}

Task<std::string> Service::pages_json_for(std::string member)
{
    const Powers p = co_await powers(member);
    const std::set<int64_t> held = co_await held_roles(db_, member);
    Result r = co_await db_->execSqlCoro("SELECT id, title, kind, updated_ms, restricted,"
                                         " (SELECT GROUP_CONCAT(pr.role ORDER BY pr.role) FROM page_roles pr WHERE pr.page = pages.id)"
                                         " FROM pages ORDER BY position, id");
    std::string out = "[";
    for (const auto &row : r) {
        const bool restricted = num(row[4]) != 0;
        const std::vector<int64_t> roles = id_vector(text(row[5]));
        if (!sees(p, held, restricted, roles))
            continue;
        out += std::string(out.size() > 1 ? "," : "") +
               page_head(num(row[0]), text(row[1]), text(row[2]), num(row[3]), restricted ? "[" + ids_text(roles) + "]" : std::string("null"));
    }
    co_return out + "]";
}

Task<bool> Service::may_see_page(std::string member, int64_t page)
{
    const bool in = co_await is_member(member);
    if (!in)
        co_return false;
    Result r = co_await db_->execSqlCoro("SELECT restricted, (SELECT GROUP_CONCAT(pr.role) FROM page_roles pr WHERE pr.page = pages.id)"
                                         " FROM pages WHERE id = ?",
                                         page);
    if (r.empty())
        co_return false;
    const Powers p = co_await powers(member);
    const std::set<int64_t> held = co_await held_roles(db_, member);
    co_return sees(p, held, num(r[0][0]) != 0, id_vector(text(r[0][1])));
}

/* What a kind adds to a page as a member reads it: the roles they hold of those offered, the events, the items */
Task<std::string> Service::page_extra(std::string member, int64_t page, std::string kind, std::string data)
{
    if (kind == "roles") {
        Json::Value v;
        std::vector<int64_t> offered;
        if (parse_object(data, v))
            for (const auto &r : v["roles"])
                offered.push_back(r["role"].asInt64());
        const std::set<int64_t> held = co_await held_roles(db_, member);
        std::vector<int64_t> mine;
        for (int64_t r : offered)
            if (held.count(r))
                mine.push_back(r);
        co_return ",\"mine\":[" + ids_text(mine) + "]";
    }
    if (kind == "events") {
        const int64_t since = now_ms() - 30LL * 24 * 3600 * 1000; /* and those of the last thirty days */
        Result r = co_await db_->execSqlCoro("SELECT e.id, e.title, e.description, e.place, e.starts_ms, e.ends_ms,"
                                             " (SELECT COUNT(*) FROM event_going g WHERE g.event = e.id),"
                                             " (SELECT COUNT(*) FROM event_going g WHERE g.event = e.id AND g.member = ?)"
                                             " FROM page_events e WHERE e.page = ? AND COALESCE(e.ends_ms, e.starts_ms) >= ? ORDER BY e.starts_ms, e.id",
                                             member, page, since);
        std::string out = ",\"events\":[";
        for (size_t i = 0; i < r.size(); i++)
            out += std::string(i ? "," : "") + "{\"id\":" + std::to_string(num(r[i][0])) + ",\"title\":" + jstr(text(r[i][1])) +
                   ",\"description\":" + jstr(text(r[i][2])) + ",\"place\":" + jstr(text(r[i][3])) + ",\"starts\":" + std::to_string(num(r[i][4])) +
                   ",\"ends\":" + (r[i][5].isNull() ? std::string("null") : std::to_string(num(r[i][5]))) + ",\"going\":" + std::to_string(num(r[i][6])) +
                   ",\"me\":" + (num(r[i][7]) ? "true" : "false") + "}";
        co_return out + "]";
    }
    if (kind == "shop") {
        const std::string items = co_await shop_items_json(page, member);
        co_return items;
    }
    co_return "";
}

Task<Answer> Service::page_get(std::string member, int64_t page)
{
    try {
        const bool seen = co_await may_see_page(member, page);
        if (!seen)
            co_return fail(404, "no_page");
        Result r = co_await db_->execSqlCoro("SELECT id, title, kind, body, updated_ms, updated_by, restricted, COALESCE(data, '{}') FROM pages WHERE id = ?", page);
        if (r.empty())
            co_return fail(404, "no_page");
        Result f = co_await db_->execSqlCoro("SELECT file, name, mime, size, w, h FROM page_files WHERE page = ? ORDER BY position", page);
        std::string files = "[";
        for (size_t i = 0; i < f.size(); i++) {
            files += std::string(i ? "," : "") + "{\"id\":" + jstr(text(f[i][0])) + ",\"name\":" + jstr(text(f[i][1])) + ",\"mime\":" +
                     jstr(text(f[i][2])) + ",\"size\":" + std::to_string(num(f[i][3]));
            if (num(f[i][4]) > 0 && num(f[i][5]) > 0)
                files += ",\"w\":" + std::to_string(num(f[i][4])) + ",\"h\":" + std::to_string(num(f[i][5]));
            files += "}";
        }
        files += "]";
        const std::string kind = text(r[0][2]), data = text(r[0][7]);
        const std::string roles = co_await page_roles_of(db_, page, num(r[0][6]) != 0);
        const std::string extra = co_await page_extra(member, page, kind, data);
        co_return Answer{200, "{\"id\":" + std::to_string(num(r[0][0])) + ",\"title\":" + jstr(text(r[0][1])) + ",\"kind\":" +
                                  jstr(kind) + ",\"body\":" + jstr(text(r[0][3])) + ",\"files\":" + files +
                                  ",\"updated\":" + std::to_string(num(r[0][4])) + ",\"updated_by\":" + jstr(text(r[0][5])) +
                                  ",\"roles\":" + roles + ",\"data\":" + data + extra + "}"};
    } catch (const DrogonDbException &e) {
        co_return server_error(e);
    }
}

/* The roles a roles page offers must be below the highest of whoever offers them, and give nothing they can't do themselves:
 * else a member who may manage pages could take more than they have. "" if so, else the error code. */
static Task<std::string> may_offer(TxPtr tx, Powers p, std::string data)
{
    Json::Value v;
    if (!parse_object(data, v))
        co_return "";
    for (const auto &e : v["roles"]) {
        Result r = co_await tx->execSqlCoro("SELECT position, permissions FROM roles WHERE id = ?", e["role"].asInt64());
        if (r.empty())
            co_return "unknown_role";
        if (!p.ranked() && (num(r[0][0]) <= p.top || (num(r[0][1]) & ~p.permissions) != 0))
            co_return "outranked";
    }
    co_return "";
}

/* who a page is for: everyone, or these roles (each must be one) */
static Task<bool> put_roles(TxPtr tx, int64_t page, std::optional<std::vector<int64_t>> roles)
{
    co_await tx->execSqlCoro("DELETE FROM page_roles WHERE page = ?", page);
    const int64_t restricted = roles ? 1 : 0;
    co_await tx->execSqlCoro("UPDATE pages SET restricted = ? WHERE id = ?", restricted, page);
    if (!roles)
        co_return true;
    std::vector<int64_t> ids = *roles;
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    for (int64_t r : ids) {
        Result q = co_await tx->execSqlCoro("SELECT 1 FROM roles WHERE id = ?", r);
        if (q.empty())
            co_return false;
        co_await tx->execSqlCoro("INSERT INTO page_roles (page, role) VALUES (?, ?)", page, r);
    }
    co_return true;
}

/* Tell members a page changed: `page` (for a Puddle of protocol 11, of a page everyone sees), and `pages`, written for each */
Task<int64_t> Service::page_changed(TxPtr tx, int64_t page, std::string title, std::string kind, int64_t updated)
{
    Result r = co_await tx->execSqlCoro("SELECT restricted FROM pages WHERE id = ?", page);
    if (!r.empty() && num(r[0][0]) == 0)
        co_await add_event(tx, "page", page_head(page, title, kind, updated));
    const int64_t ev = co_await add_event(tx, "pages", "{}");
    co_return ev;
}

Task<Answer> Service::admin_page_create(std::string by, PageEdit edit)
{
    if (!edit.title)
        co_return fail(400, "bad_request");
    std::string why = check(edit);
    if (!why.empty())
        co_return refused(why);
    const std::string kind = edit.kind.value_or("markdown"), body = edit.body.value_or("");
    std::string data = "{}";
    why = clean_data(kind, edit.data.value_or("{}"), data);
    if (!why.empty())
        co_return bad(why);
    const Powers p = co_await powers(by);
    TxPtr tx;
    int64_t id = 0, now = now_ms();
    try {
        tx = co_await db_->newTransactionCoro();
        Result c = co_await tx->execSqlCoro("SELECT COUNT(*), COALESCE(MAX(position) + 1, 0) FROM pages FOR UPDATE");
        if (num(c[0][0]) >= PAGES_MAX) {
            tx->rollback();
            co_return bad("a Pond has at most " + std::to_string(PAGES_MAX) + " pages");
        }
        const std::string offer = co_await may_offer(tx, p, data);
        if (!offer.empty()) {
            tx->rollback();
            const int status = offer == "outranked" ? 409 : 404;
            co_return fail(status, offer.c_str());
        }
        Result r = co_await tx->execSqlCoro("INSERT INTO pages (title, kind, body, position, created_ms, updated_ms, updated_by, data)"
                                            " VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
                                            *edit.title, kind, body, num(c[0][1]), now, now, by, data);
        id = (int64_t)r.insertId();
        std::string missing;
        const bool put = co_await put_files(tx, id, edit.files.value_or(std::vector<FileRef>{}), &missing);
        if (!put) {
            tx->rollback();
            co_return Answer{409, "{\"error\":\"no_file\",\"file\":" + jstr(missing) + "}"}; /* not here whole: send it first */
        }
        const bool roles_ok = co_await put_roles(tx, id, edit.roles.value_or(std::nullopt));
        if (!roles_ok) {
            tx->rollback();
            co_return fail(404, "unknown_role");
        }
        int64_t ev = co_await page_changed(tx, id, *edit.title, kind, now);
        if (!co_await Committed(std::move(tx)))
            co_return fail(500, "database");
        bump(ev);
    } catch (const DrogonDbException &e) {
        if (tx)
            tx->rollback();
        co_return server_error(e);
    }
    co_await log_admin(by, "page_create", *edit.title, kind);
    co_return Answer{200, page_head(id, *edit.title, kind, now)};
}

Task<Answer> Service::admin_page_edit(std::string by, int64_t page, PageEdit edit)
{
    if (!edit.title && !edit.body && !edit.files && !edit.kind && !edit.data && !edit.roles)
        co_return fail(400, "bad_request");
    const std::string why = check(edit);
    if (!why.empty())
        co_return refused(why);
    const Powers p = co_await powers(by);
    TxPtr tx;
    std::string title, kind;
    int64_t now = now_ms();
    try {
        tx = co_await db_->newTransactionCoro();
        Result r = co_await tx->execSqlCoro("SELECT title, kind, COALESCE(data, '{}') FROM pages WHERE id = ? FOR UPDATE", page);
        if (r.empty()) {
            tx->rollback();
            co_return fail(404, "no_page");
        }
        title = edit.title.value_or(text(r[0][0]));
        kind = edit.kind.value_or(text(r[0][1]));
        if (edit.data || edit.kind) { /* a new kind starts from its own settings */
            std::string data;
            const std::string given = edit.data ? *edit.data : (kind == text(r[0][1]) ? text(r[0][2]) : std::string("{}"));
            const std::string bad_data = clean_data(kind, given, data);
            if (!bad_data.empty()) {
                tx->rollback();
                co_return bad(bad_data);
            }
            const std::string offer = co_await may_offer(tx, p, data);
            if (!offer.empty()) {
                tx->rollback();
                const int status = offer == "outranked" ? 409 : 404;
            co_return fail(status, offer.c_str());
            }
            co_await tx->execSqlCoro("UPDATE pages SET data = ? WHERE id = ?", data, page);
        }
        if (edit.body)
            co_await tx->execSqlCoro("UPDATE pages SET body = ? WHERE id = ?", *edit.body, page);
        co_await tx->execSqlCoro("UPDATE pages SET title = ?, kind = ?, updated_ms = ?, updated_by = ? WHERE id = ?", title, kind, now, by, page);
        if (edit.files) {
            std::string missing;
            const bool put = co_await put_files(tx, page, *edit.files, &missing);
            if (!put) {
                tx->rollback();
                co_return Answer{409, "{\"error\":\"no_file\",\"file\":" + jstr(missing) + "}"};
            }
        }
        if (edit.roles) {
            const bool roles_ok = co_await put_roles(tx, page, *edit.roles);
            if (!roles_ok) {
                tx->rollback();
                co_return fail(404, "unknown_role");
            }
        }
        int64_t ev = co_await page_changed(tx, page, title, kind, now);
        if (!co_await Committed(std::move(tx)))
            co_return fail(500, "database");
        bump(ev);
    } catch (const DrogonDbException &e) {
        if (tx)
            tx->rollback();
        co_return server_error(e);
    }
    std::string what;
    if (edit.body)
        what = std::to_string(edit.body->size()) + " bytes of markdown";
    if (edit.files)
        what += (what.empty() ? "" : ", ") + std::to_string(edit.files->size()) + " files";
    co_await log_admin(by, "page_edit", title, what);
    co_return Answer{204, ""};
}

/* Its files are let go with it: those no message or other page carries go in the hourly sweep. */
Task<Answer> Service::admin_page_delete(std::string by, int64_t page)
{
    TxPtr tx;
    std::string was;
    try {
        tx = co_await db_->newTransactionCoro();
        Result r = co_await tx->execSqlCoro("SELECT title FROM pages WHERE id = ? FOR UPDATE", page);
        if (r.empty()) {
            tx->rollback();
            co_return fail(404, "no_page");
        }
        was = text(r[0][0]);
        co_await tx->execSqlCoro("DELETE FROM page_files WHERE page = ?", page);
        co_await tx->execSqlCoro("DELETE FROM page_roles WHERE page = ?", page);
        co_await tx->execSqlCoro("DELETE FROM event_going WHERE event IN (SELECT id FROM page_events WHERE page = ?)", page);
        co_await tx->execSqlCoro("DELETE FROM page_events WHERE page = ?", page);
        co_await tx->execSqlCoro("UPDATE shop_items SET active = 0, page = 0 WHERE page = ?", page); /* orders still name them */
        co_await tx->execSqlCoro("DELETE FROM pages WHERE id = ?", page);
        co_await add_event(tx, "page_deleted", "{\"id\":" + std::to_string(page) + "}");
        int64_t ev = co_await add_event(tx, "pages", "{}");
        if (!co_await Committed(std::move(tx)))
            co_return fail(500, "database");
        bump(ev);
    } catch (const DrogonDbException &e) {
        if (tx)
            tx->rollback();
        co_return server_error(e);
    }
    co_await log_admin(by, "page_delete", was, "");
    co_return Answer{204, ""};
}

Task<Answer> Service::admin_page_order(std::string by, std::vector<int64_t> pages)
{
    TxPtr tx;
    try {
        tx = co_await db_->newTransactionCoro();
        Result r = co_await tx->execSqlCoro("SELECT id FROM pages FOR UPDATE");
        std::vector<int64_t> live, asked = pages;
        for (const auto &row : r)
            live.push_back(num(row[0]));
        std::sort(live.begin(), live.end());
        std::sort(asked.begin(), asked.end());
        if (live != asked) { /* each page once: one made or deleted meanwhile is a conflict, to ask again */
            tx->rollback();
            co_return fail(409, "pages_changed");
        }
        for (size_t i = 0; i < pages.size(); i++)
            co_await tx->execSqlCoro("UPDATE pages SET position = ? WHERE id = ?", (int64_t)i, pages[i]);
        co_await add_event(tx, "page_order", "{\"pages\":[" + ids_text(pages) + "]}");
        int64_t ev = co_await add_event(tx, "pages", "{}");
        if (!co_await Committed(std::move(tx)))
            co_return fail(500, "database");
        bump(ev);
    } catch (const DrogonDbException &e) {
        if (tx)
            tx->rollback();
        co_return server_error(e);
    }
    co_await log_admin(by, "page_order", "", ids_text(pages));
    co_return Answer{204, ""};
}

/* ---- roles to pick ---- */

Task<Answer> Service::page_pick_roles(std::string member, int64_t page, std::vector<int64_t> roles)
{
    std::sort(roles.begin(), roles.end());
    roles.erase(std::unique(roles.begin(), roles.end()), roles.end());
    TxPtr tx;
    try {
        const bool seen = co_await may_see_page(member, page);
        if (!seen)
            co_return fail(404, "no_page");
        tx = co_await db_->newTransactionCoro();
        Result r = co_await tx->execSqlCoro("SELECT kind, COALESCE(data, '{}') FROM pages WHERE id = ? FOR UPDATE", page);
        if (r.empty() || text(r[0][0]) != "roles") {
            tx->rollback();
            co_return fail(404, "no_page");
        }
        Json::Value v;
        parse_object(text(r[0][1]), v);
        std::set<int64_t> offered;
        for (const auto &e : v["roles"])
            offered.insert(e["role"].asInt64());
        for (int64_t want : roles)
            if (!offered.count(want)) {
                tx->rollback();
                co_return fail(409, "not_offered");
            }
        if (!v.get("multiple", true).asBool() && roles.size() > 1) {
            tx->rollback();
            co_return fail(409, "one_only");
        }
        for (int64_t o : offered)
            co_await tx->execSqlCoro("DELETE FROM member_roles WHERE member = ? AND role = ?", member, o);
        for (int64_t want : roles) {
            Result q = co_await tx->execSqlCoro("SELECT 1 FROM roles WHERE id = ?", want);
            if (!q.empty())
                co_await tx->execSqlCoro("INSERT INTO member_roles (member, role) VALUES (?, ?)", member, want);
        }
        const std::string mj = co_await member_json_of(tx, member);
        co_await add_event(tx, "member", mj);
        co_await add_event(tx, "channels", "{}", member);
        const int64_t ev = co_await add_event(tx, "pages", "{}", member);
        if (!co_await Committed(std::move(tx)))
            co_return fail(500, "database");
        bump(ev);
    } catch (const DrogonDbException &e) {
        if (tx)
            tx->rollback();
        co_return server_error(e);
    }
    co_return Answer{200, "{\"roles\":[" + ids_text(roles) + "]}"};
}

/* ---- events ---- */

/* `e` checked against what is there (`was`, for a change): "" if it may be set, else why not */
static std::string check_event(EventEdit &e, bool creating)
{
    std::string t;
    if (creating && (!e.title || !e.starts))
        return "an event has a title and a start";
    if (e.title) {
        if (!clean_text(*e.title, EVENT_TITLE_MAX, false, t) || t.empty())
            return "an event's title is 1 to 100 characters on one line";
        e.title = t;
    }
    if (e.description) {
        if (!clean_text(*e.description, EVENT_TEXT_MAX, true, t))
            return "an event's description is at most 4000 characters";
        e.description = t;
    }
    if (e.place) {
        if (!clean_text(*e.place, EVENT_PLACE_MAX, false, t))
            return "an event's place is one line of up to 200 characters";
        e.place = t;
    }
    if (e.starts && *e.starts < 1)
        return "an event starts at a time";
    return "";
}

static Task<bool> page_is(TxPtr tx, int64_t page, std::string kind)
{
    Result r = co_await tx->execSqlCoro("SELECT kind FROM pages WHERE id = ? FOR UPDATE", page);
    co_return !r.empty() && text(r[0][0]) == kind;
}

/* A page's events or items changed: it is newer, so those reading it read it again */
Task<int64_t> Service::page_touched(TxPtr tx, int64_t page, std::string by)
{
    const int64_t now = now_ms();
    co_await tx->execSqlCoro("UPDATE pages SET updated_ms = ? WHERE id = ?", now, page);
    Result r = co_await tx->execSqlCoro("SELECT title, kind FROM pages WHERE id = ?", page);
    const int64_t ev = co_await page_changed(tx, page, text(r[0][0]), text(r[0][1]), now);
    co_return ev;
}

Task<Answer> Service::admin_event_create(std::string by, int64_t page, EventEdit e)
{
    const std::string why = check_event(e, true);
    if (!why.empty())
        co_return bad(why);
    if (e.ends && *e.ends && *e.ends < *e.starts)
        co_return bad("an event ends after it starts");
    TxPtr tx;
    int64_t id = 0;
    try {
        tx = co_await db_->newTransactionCoro();
        const bool events = co_await page_is(tx, page, "events");
        if (!events) {
            tx->rollback();
            co_return fail(404, "no_page");
        }
        Result c = co_await tx->execSqlCoro("SELECT COUNT(*) FROM page_events WHERE page = ?", page);
        if (num(c[0][0]) >= EVENTS_PER_PAGE_MAX) {
            tx->rollback();
            co_return bad("a page has at most 500 events");
        }
        const std::optional<int64_t> ends = e.ends && *e.ends ? e.ends : std::nullopt;
        Result r = co_await tx->execSqlCoro("INSERT INTO page_events (page, title, description, place, starts_ms, ends_ms, created_by, created_ms)"
                                            " VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
                                            page, *e.title, e.description.value_or(""), e.place.value_or(""), *e.starts, ends, by, now_ms());
        id = (int64_t)r.insertId();
        const int64_t ev = co_await page_touched(tx, page, by);
        if (!co_await Committed(std::move(tx)))
            co_return fail(500, "database");
        bump(ev);
    } catch (const DrogonDbException &ex) {
        if (tx)
            tx->rollback();
        co_return server_error(ex);
    }
    co_await log_admin(by, "event_create", *e.title, std::to_string(*e.starts));
    co_return Answer{200, "{\"id\":" + std::to_string(id) + "}"};
}

Task<Answer> Service::admin_event_edit(std::string by, int64_t page, int64_t event, EventEdit e)
{
    const std::string why = check_event(e, false);
    if (!why.empty())
        co_return bad(why);
    TxPtr tx;
    std::string title;
    try {
        tx = co_await db_->newTransactionCoro();
        Result r = co_await tx->execSqlCoro("SELECT title, description, place, starts_ms, ends_ms FROM page_events WHERE id = ? AND page = ? FOR UPDATE",
                                            event, page);
        if (r.empty()) {
            tx->rollback();
            co_return fail(404, "no_event");
        }
        title = e.title.value_or(text(r[0][0]));
        const int64_t starts = e.starts.value_or(num(r[0][3]));
        std::optional<int64_t> ends = r[0][4].isNull() ? std::nullopt : std::optional<int64_t>(num(r[0][4]));
        if (e.ends)
            ends = *e.ends ? e.ends : std::nullopt;
        if (ends && *ends < starts) {
            tx->rollback();
            co_return bad("an event ends after it starts");
        }
        co_await tx->execSqlCoro("UPDATE page_events SET title = ?, description = ?, place = ?, starts_ms = ?, ends_ms = ? WHERE id = ?", title,
                                 e.description.value_or(text(r[0][1])), e.place.value_or(text(r[0][2])), starts, ends, event);
        const int64_t ev = co_await page_touched(tx, page, by);
        if (!co_await Committed(std::move(tx)))
            co_return fail(500, "database");
        bump(ev);
    } catch (const DrogonDbException &ex) {
        if (tx)
            tx->rollback();
        co_return server_error(ex);
    }
    co_await log_admin(by, "event_edit", title, "");
    co_return Answer{204, ""};
}

Task<Answer> Service::admin_event_delete(std::string by, int64_t page, int64_t event)
{
    TxPtr tx;
    std::string title;
    try {
        tx = co_await db_->newTransactionCoro();
        Result r = co_await tx->execSqlCoro("SELECT title FROM page_events WHERE id = ? AND page = ? FOR UPDATE", event, page);
        if (r.empty()) {
            tx->rollback();
            co_return fail(404, "no_event");
        }
        title = text(r[0][0]);
        co_await tx->execSqlCoro("DELETE FROM event_going WHERE event = ?", event);
        co_await tx->execSqlCoro("DELETE FROM page_events WHERE id = ?", event);
        const int64_t ev = co_await page_touched(tx, page, by);
        if (!co_await Committed(std::move(tx)))
            co_return fail(500, "database");
        bump(ev);
    } catch (const DrogonDbException &ex) {
        if (tx)
            tx->rollback();
        co_return server_error(ex);
    }
    co_await log_admin(by, "event_delete", title, "");
    co_return Answer{204, ""};
}

Task<Answer> Service::event_going(std::string member, int64_t page, int64_t event, bool going)
{
    try {
        const bool seen = co_await may_see_page(member, page);
        if (!seen)
            co_return fail(404, "no_page");
        Result r = co_await db_->execSqlCoro("SELECT 1 FROM page_events WHERE id = ? AND page = ?", event, page);
        if (r.empty())
            co_return fail(404, "no_event");
        if (going)
            co_await db_->execSqlCoro("INSERT IGNORE INTO event_going (event, member) VALUES (?, ?)", event, member);
        else
            co_await db_->execSqlCoro("DELETE FROM event_going WHERE event = ? AND member = ?", event, member);
        Result c = co_await db_->execSqlCoro("SELECT COUNT(*) FROM event_going WHERE event = ?", event);
        co_return Answer{200, std::string("{\"going\":") + (going ? "true" : "false") + ",\"count\":" + std::to_string(num(c[0][0])) + "}"};
    } catch (const DrogonDbException &ex) {
        co_return server_error(ex);
    }
}

} // namespace pond
