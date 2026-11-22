/* Pages (docs/design.md, "Pages"): what a Pond shows its members above the channels, as Discord's server guide is: rules, a list
 * of servers, a guide. A page is a title, a kind and a body, in an order the admins set; a markdown page may carry files, which
 * are sent to the Pond first as a message's are. Every member sees every page; those who may manage pages (operators,
 * administrators, members whose roles say `manage_pages`) make, change, order and delete them.
 *
 * The kind is where later pages come in (roles to pick, events, an entry form, a shop): each will be a kind of its own, with
 * its own body. Only "markdown" is made yet. */

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

static std::string ids_text(const std::vector<int64_t> &ids)
{
    std::string out;
    for (int64_t id : ids)
        out += (out.empty() ? "" : ",") + std::to_string(id);
    return out;
}

/* {id, title, kind, updated}: as /info lists a page, and the `page` event says one */
static std::string page_head(int64_t id, const std::string &title, const std::string &kind, int64_t updated)
{
    return "{\"id\":" + std::to_string(id) + ",\"title\":" + jstr(title) + ",\"kind\":" + jstr(kind) + ",\"updated\":" + std::to_string(updated) + "}";
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
    if (why == "unknown_kind" || why == "bad_file")
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

Task<std::string> Service::pages_json()
{
    Result r = co_await db_->execSqlCoro("SELECT id, title, kind, updated_ms FROM pages ORDER BY position, id");
    std::string out = "[";
    for (size_t i = 0; i < r.size(); i++)
        out += std::string(i ? "," : "") + page_head(num(r[i][0]), text(r[i][1]), text(r[i][2]), num(r[i][3]));
    co_return out + "]";
}

Task<Answer> Service::page_get(std::string member, int64_t page)
{
    try {
        if (!co_await is_member(member))
            co_return fail(404, "no_page");
        Result r = co_await db_->execSqlCoro("SELECT id, title, kind, body, updated_ms, updated_by FROM pages WHERE id = ?", page);
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
        co_return Answer{200, "{\"id\":" + std::to_string(num(r[0][0])) + ",\"title\":" + jstr(text(r[0][1])) + ",\"kind\":" +
                                  jstr(text(r[0][2])) + ",\"body\":" + jstr(text(r[0][3])) + ",\"files\":" + files +
                                  ",\"updated\":" + std::to_string(num(r[0][4])) + ",\"updated_by\":" + jstr(text(r[0][5])) + "}"};
    } catch (const DrogonDbException &e) {
        co_return server_error(e);
    }
}

Task<Answer> Service::admin_page_create(std::string by, PageEdit edit)
{
    if (!edit.title)
        co_return fail(400, "bad_request");
    const std::string why = check(edit);
    if (!why.empty())
        co_return refused(why);
    const std::string kind = edit.kind.value_or("markdown"), body = edit.body.value_or("");
    TxPtr tx;
    int64_t id = 0, now = now_ms();
    try {
        tx = co_await db_->newTransactionCoro();
        Result c = co_await tx->execSqlCoro("SELECT COUNT(*), COALESCE(MAX(position) + 1, 0) FROM pages FOR UPDATE");
        if (num(c[0][0]) >= PAGES_MAX) {
            tx->rollback();
            co_return bad("a Pond has at most " + std::to_string(PAGES_MAX) + " pages");
        }
        Result r = co_await tx->execSqlCoro("INSERT INTO pages (title, kind, body, position, created_ms, updated_ms, updated_by)"
                                            " VALUES (?, ?, ?, ?, ?, ?, ?)",
                                            *edit.title, kind, body, num(c[0][1]), now, now, by);
        id = (int64_t)r.insertId();
        std::string missing;
        const bool put = co_await put_files(tx, id, edit.files.value_or(std::vector<FileRef>{}), &missing);
        if (!put) {
            tx->rollback();
            co_return Answer{409, "{\"error\":\"no_file\",\"file\":" + jstr(missing) + "}"}; /* not here whole: send it first */
        }
        int64_t ev = co_await add_event(tx, "page", page_head(id, *edit.title, kind, now));
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
    if (!edit.title && !edit.body && !edit.files && !edit.kind)
        co_return fail(400, "bad_request");
    const std::string why = check(edit);
    if (!why.empty())
        co_return refused(why);
    TxPtr tx;
    std::string title, kind;
    int64_t now = now_ms();
    try {
        tx = co_await db_->newTransactionCoro();
        Result r = co_await tx->execSqlCoro("SELECT title, kind FROM pages WHERE id = ? FOR UPDATE", page);
        if (r.empty()) {
            tx->rollback();
            co_return fail(404, "no_page");
        }
        title = edit.title.value_or(text(r[0][0]));
        kind = edit.kind.value_or(text(r[0][1]));
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
        int64_t ev = co_await add_event(tx, "page", page_head(page, title, kind, now));
        if (!co_await Committed(std::move(tx)))
            co_return fail(500, "database");
        bump(ev);
    } catch (const DrogonDbException &e) {
        if (tx)
            tx->rollback();
        co_return server_error(e);
    }
    co_await log_admin(by, "page_edit", title, "");
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
        co_await tx->execSqlCoro("DELETE FROM pages WHERE id = ?", page);
        int64_t ev = co_await add_event(tx, "page_deleted", "{\"id\":" + std::to_string(page) + "}");
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
        int64_t ev = co_await add_event(tx, "page_order", "{\"pages\":[" + ids_text(pages) + "]}");
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

} // namespace pond
