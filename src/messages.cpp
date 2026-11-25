/* Editing and deleting messages (docs/design.md, "Editing and deleting messages"). A member edits their own messages: the text,
 * and dropping some of the files. Their author, or whoever may manage messages in the channel, deletes one: it is gone from the
 * channel for every member, but the Pond keeps it, with every text it had and its files, in the message log, which those who may
 * view it read. Purging erases a deleted message for good, and its files with it unless something else carries them.
 *
 * The events about a message (`message`, `message_edited`) are rewritten when it changes, so a member catching up never reads
 * what was taken back: an edit makes them all say the message as it reads now, and a deletion turns them into
 * `message_deleted`. */

#include "pond.h"

#include "json.h"
#include "text.h"

#include <drogon/orm/Exception.h>
#include <trantor/utils/Logger.h>

#include <algorithm>
#include <map>
#include <set>
#include <unistd.h>

namespace pond {

using drogon::orm::DrogonDbException;
using drogon::orm::Result;

/* as pond.cpp's MESSAGE_COLS, for messages_json(); and what the log adds */
static const char COLS[] = "id, channel, author, text, ts_ms, edited_ms";
static const char LOG_COLS[] = "id, channel, author, text, ts_ms, edited_ms, deleted_ms, deleted_by";

static Answer server_error(const DrogonDbException &e)
{
    LOG_ERROR << "database: " << e.base().what();
    return fail(500, "database");
}

static std::string deleted_json(int64_t id, int64_t channel)
{
    return "{\"id\":" + std::to_string(id) + ",\"channel\":" + std::to_string(channel) + "}";
}

static Answer muted_answer(int64_t until)
{
    return Answer{409, "{\"error\":\"muted\",\"until\":" + (until ? std::to_string(until) : std::string("null")) + "}"};
}

Task<Answer> Service::message_edit(std::string member, int64_t channel, int64_t message, std::optional<std::string> in,
                                   std::optional<std::vector<std::string>> keep)
{
    std::optional<std::string> body;
    if (in) {
        std::string cleaned;
        if (!clean_text(*in, TEXT_MAX, true, cleaned))
            co_return fail(400, "bad_text");
        body = cleaned;
    }
    if (keep && keep->size() > FILES_PER_MESSAGE)
        co_return fail(400, "bad_file");
    if (!send_limit_.take(member, now_ms()))
        co_return fail(429, "slow_down");
    if (int64_t until = muted_until(member); until >= 0) /* muted: nothing they write changes (docs/design.md, "Muting") */
        co_return muted_answer(until);
    TxPtr tx;
    try {
        Result live = co_await db_->execSqlCoro("SELECT 1 FROM channels WHERE id = ? AND deleted = 0", channel);
        const int64_t may = co_await permissions_in(member, channel);
        if (live.empty() || !(may & VIEW_CHANNELS))
            co_return fail(404, "no_channel");
        tx = co_await db_->newTransactionCoro();
        Result m = co_await tx->execSqlCoro("SELECT author, text, ts_ms, edited_ms FROM messages WHERE id = ? AND channel = ? AND deleted_ms IS NULL"
                                            " FOR UPDATE",
                                            message, channel);
        if (m.empty() || text(m[0][0]) != member) { /* only its author edits a message */
            tx->rollback();
            co_return fail(404, "no_message");
        }
        const std::string was = text(m[0][1]);
        const int64_t since = m[0][3].isNull() ? num(m[0][2]) : num(m[0][3]);
        Result f = co_await tx->execSqlCoro("SELECT file FROM message_files WHERE message = ? AND removed_ms IS NULL ORDER BY position", message);
        std::vector<std::string> had, drop;
        for (const auto &row : f)
            had.push_back(text(row[0]));
        size_t kept = had.size();
        if (keep) {
            std::set<std::string> want(keep->begin(), keep->end());
            for (const std::string &id : want)
                if (std::find(had.begin(), had.end(), id) == had.end()) { /* an edit only drops files, never adds one */
                    tx->rollback();
                    co_return fail(400, "bad_file");
                }
            for (const std::string &id : had)
                if (!want.count(id))
                    drop.push_back(id);
            kept = want.size();
        }
        const std::string now_text = body.value_or(was);
        if (now_text.empty() && kept == 0) {
            tx->rollback();
            co_return fail(400, "bad_text");
        }
        const int64_t now = now_ms();
        const bool changed = now_text != was || !drop.empty();
        if (changed) {
            /* what it said until now goes in the log, with the files it carried then (those not dropped before `now`) */
            co_await tx->execSqlCoro("INSERT INTO message_versions (message, text, ts_ms, until_ms) VALUES (?, ?, ?, ?)", message, was,
                                     since, now);
            co_await tx->execSqlCoro("UPDATE messages SET text = ?, edited_ms = ? WHERE id = ?", now_text, now, message);
            for (const std::string &id : drop)
                co_await tx->execSqlCoro("UPDATE message_files SET removed_ms = ? WHERE message = ? AND file = ? AND removed_ms IS NULL", now,
                                         message, id);
        }
        Result r = co_await tx->execSqlCoro(std::string("SELECT ") + COLS + " FROM messages WHERE id = ?", message);
        std::string list = co_await messages_json(tx, r);
        std::string msg = list.substr(1, list.size() - 2);
        if (!changed) { /* the same as it was: nothing to tell anyone */
            tx->rollback();
            co_return Answer{200, msg};
        }
        /* what was said of it before says it as it reads now, so nobody catching up reads what was taken back */
        co_await tx->execSqlCoro("UPDATE events SET data = ? WHERE message = ? AND kind IN ('message', 'message_edited')", msg, message);
        const int64_t ev = co_await add_event(tx, "message_edited", msg, "", channel, message);
        if (!co_await Committed(std::move(tx)))
            co_return fail(500, "database");
        bump(ev);
        co_return Answer{200, msg};
    } catch (const DrogonDbException &e) {
        if (tx)
            tx->rollback();
        co_return server_error(e);
    }
}

Task<Answer> Service::delete_message(std::string by, int64_t message)
{
    TxPtr tx;
    try {
        tx = co_await db_->newTransactionCoro();
        Result m = co_await tx->execSqlCoro("SELECT channel FROM messages WHERE id = ? AND deleted_ms IS NULL FOR UPDATE", message);
        if (m.empty()) {
            tx->rollback();
            co_return fail(404, "no_message");
        }
        const int64_t channel = num(m[0][0]);
        const std::string gone = deleted_json(message, channel);
        co_await tx->execSqlCoro("UPDATE messages SET deleted_ms = ?, deleted_by = ? WHERE id = ?", now_ms(), by, message);
        /* what was said of it before is taken back too: its events say only that it is gone */
        co_await tx->execSqlCoro("UPDATE events SET kind = 'message_deleted', data = ? WHERE message = ?", gone, message);
        const int64_t ev = co_await add_event(tx, "message_deleted", gone, "", channel, message);
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

Task<Answer> Service::message_delete(std::string member, int64_t channel, int64_t message)
{
    std::string author;
    try {
        Result live = co_await db_->execSqlCoro("SELECT 1 FROM channels WHERE id = ? AND deleted = 0", channel);
        const int64_t may = co_await permissions_in(member, channel);
        if (live.empty() || !(may & VIEW_CHANNELS))
            co_return fail(404, "no_channel");
        Result m = co_await db_->execSqlCoro("SELECT author FROM messages WHERE id = ? AND channel = ? AND deleted_ms IS NULL", message, channel);
        if (m.empty())
            co_return fail(404, "no_message");
        author = text(m[0][0]);
        /* their own, or anyone's where they may manage messages (no rank between them: as Discord's) */
        if (author != member && !(may & MANAGE_MESSAGES))
            co_return fail(409, "no_permission");
    } catch (const DrogonDbException &e) {
        co_return server_error(e);
    }
    Answer a = co_await delete_message(member, message);
    if (a.status == 204 && author != member)
        co_await log_admin(member, "message_delete", author, "message " + std::to_string(message) + " in channel " + std::to_string(channel));
    co_return a;
}

/* Rows of LOG_COLS, each with every text it had and every file it carried:
 * {id, channel, author, text, ts, edited?, deleted?: {ts, by}, files: [{..., removed?}], versions: [{text, ts, until, files: [ids]}]}.
 * `text` and `files` are the last it had; `versions` the earlier ones, oldest first. */
Task<std::string> Service::logged_json(DbPtr db, Result r)
{
    if (r.empty())
        co_return "[]";
    std::string ids;
    for (size_t i = 0; i < r.size(); i++)
        ids += (i ? "," : "") + std::to_string(num(r[i][0])); /* numbers from the database: safe to put in the statement */
    struct File {
        std::string id, json;
        int64_t removed = 0; /* 0: it still carries it */
    };
    std::map<int64_t, std::vector<File>> files;
    Result f = co_await db->execSqlCoro("SELECT message, file, name, mime, size, w, h, duration_ms, preview, removed_ms FROM message_files"
                                        " WHERE message IN (" + ids + ") ORDER BY message, position");
    for (const auto &row : f) {
        File one{text(row[1]), "", num(row[9])};
        one.json = "{\"id\":" + jstr(one.id) + ",\"name\":" + jstr(text(row[2])) + ",\"mime\":" + jstr(text(row[3])) +
                   ",\"size\":" + std::to_string(num(row[4]));
        if (num(row[5]) > 0 && num(row[6]) > 0)
            one.json += ",\"w\":" + std::to_string(num(row[5])) + ",\"h\":" + std::to_string(num(row[6]));
        if (num(row[7]) > 0)
            one.json += ",\"duration_ms\":" + std::to_string(num(row[7]));
        if (!text(row[8]).empty())
            one.json += ",\"preview\":" + jstr(text(row[8]));
        if (one.removed)
            one.json += ",\"removed\":" + std::to_string(one.removed);
        one.json += "}";
        files[num(row[0])].push_back(std::move(one));
    }
    std::map<int64_t, std::string> versions;
    Result v = co_await db->execSqlCoro("SELECT message, text, ts_ms, until_ms FROM message_versions WHERE message IN (" + ids +
                                        ") ORDER BY message, id");
    for (const auto &row : v) {
        const int64_t until = num(row[3]);
        std::string carried;
        for (const File &one : files[num(row[0])])
            if (!one.removed || one.removed >= until) /* dropped at `until` or later: it still carried it then */
                carried += (carried.empty() ? "" : ",") + jstr(one.id);
        std::string &out = versions[num(row[0])];
        out += (out.empty() ? "" : ",") + std::string("{\"text\":") + jstr(text(row[1])) + ",\"ts\":" + std::to_string(num(row[2])) +
               ",\"until\":" + std::to_string(until) + ",\"files\":[" + carried + "]}";
    }
    std::string out = "[";
    for (size_t i = 0; i < r.size(); i++) {
        const auto &row = r[i];
        const int64_t id = num(row[0]);
        out += std::string(i ? "," : "") + "{\"id\":" + std::to_string(id) + ",\"channel\":" + std::to_string(num(row[1])) +
               ",\"author\":" + jstr(text(row[2])) + ",\"text\":" + jstr(text(row[3])) + ",\"ts\":" + std::to_string(num(row[4]));
        if (!row[5].isNull())
            out += ",\"edited\":" + std::to_string(num(row[5]));
        if (!row[6].isNull())
            out += ",\"deleted\":{\"ts\":" + std::to_string(num(row[6])) + ",\"by\":" + jstr(text(row[7])) + "}";
        std::string all;
        for (const File &one : files[id])
            all += (all.empty() ? "" : ",") + one.json;
        out += ",\"files\":[" + all + "],\"versions\":[" + versions[id] + "]}";
    }
    co_return out + "]";
}

Task<Answer> Service::message_log(std::string member, int64_t channel, int64_t before, int limit)
{
    try {
        Result live = co_await db_->execSqlCoro("SELECT 1 FROM channels WHERE id = ? AND deleted = 0", channel);
        const int64_t may = co_await permissions_in(member, channel);
        if (live.empty() || !(may & VIEW_CHANNELS) || !(may & VIEW_MESSAGE_LOG))
            co_return fail(404, "no_channel");
        Result r = co_await db_->execSqlCoro(std::string("SELECT ") + LOG_COLS +
                                                 " FROM messages WHERE channel = ? AND id < ? AND (edited_ms IS NOT NULL OR deleted_ms IS NOT NULL)"
                                                 " ORDER BY id DESC LIMIT ?",
                                             channel, before > 0 ? before : INT64_MAX, (int64_t)limit);
        const std::string list = co_await logged_json(db_, r);
        co_return Answer{200, "{\"messages\":" + list + "}"};
    } catch (const DrogonDbException &e) {
        co_return server_error(e);
    }
}

Task<Answer> Service::message_versions(std::string member, int64_t channel, int64_t message)
{
    try {
        Result live = co_await db_->execSqlCoro("SELECT 1 FROM channels WHERE id = ? AND deleted = 0", channel);
        const int64_t may = co_await permissions_in(member, channel);
        if (live.empty() || !(may & VIEW_CHANNELS) || !(may & VIEW_MESSAGE_LOG))
            co_return fail(404, "no_channel");
        Result r = co_await db_->execSqlCoro(std::string("SELECT ") + LOG_COLS + " FROM messages WHERE id = ? AND channel = ?", message, channel);
        if (r.empty())
            co_return fail(404, "no_message");
        const std::string list = co_await logged_json(db_, r);
        co_return Answer{200, list.substr(1, list.size() - 2)};
    } catch (const DrogonDbException &e) {
        co_return server_error(e);
    }
}

Task<Answer> Service::purge_message(std::string by, int64_t message)
{
    TxPtr tx;
    std::vector<std::string> carried;
    try {
        tx = co_await db_->newTransactionCoro();
        Result m = co_await tx->execSqlCoro("SELECT channel FROM messages WHERE id = ? FOR UPDATE", message);
        if (m.empty()) {
            tx->rollback();
            co_return fail(404, "no_message");
        }
        const std::string gone = deleted_json(message, num(m[0][0]));
        Result f = co_await tx->execSqlCoro("SELECT DISTINCT file FROM message_files WHERE message = ?", message);
        for (const auto &row : f)
            carried.push_back(text(row[0]));
        co_await tx->execSqlCoro("DELETE FROM message_versions WHERE message = ?", message);
        co_await tx->execSqlCoro("DELETE FROM message_files WHERE message = ?", message);
        co_await tx->execSqlCoro("DELETE FROM messages WHERE id = ?", message);
        co_await tx->execSqlCoro("UPDATE events SET kind = 'message_deleted', data = ? WHERE message = ?", gone, message);
        if (!co_await Committed(std::move(tx)))
            co_return fail(500, "database");
    } catch (const DrogonDbException &e) {
        if (tx)
            tx->rollback();
        co_return server_error(e);
    }
    /* its files go now, not in the hourly sweep, unless another message or a page carries them */
    int erased = 0;
    for (const std::string &id : carried) {
        try {
            Result d = co_await db_->execSqlCoro("DELETE FROM files WHERE id = ? AND NOT EXISTS (SELECT 1 FROM message_files WHERE file = ?)"
                                                 " AND NOT EXISTS (SELECT 1 FROM page_files WHERE file = ?)",
                                                 id, id, id);
            if (d.affectedRows() == 1) {
                unlink(file_path(id, false).c_str());
                unlink(file_path(id, true).c_str());
                erased++;
            }
        } catch (const DrogonDbException &e) {
            LOG_ERROR << "purge: " << e.base().what();
        }
    }
    LOG_INFO << "purged message " << message << " (" << by << "), and " << erased << " file(s) of its " << carried.size();
    co_return Answer{204, ""};
}

Task<Answer> Service::message_purge(std::string member, int64_t channel, int64_t message)
{
    std::string author;
    try {
        Result live = co_await db_->execSqlCoro("SELECT 1 FROM channels WHERE id = ? AND deleted = 0", channel);
        const int64_t may = co_await permissions_in(member, channel);
        if (live.empty() || !(may & VIEW_CHANNELS) || !(may & VIEW_MESSAGE_LOG) || !(may & MANAGE_MESSAGES))
            co_return fail(404, "no_channel");
        Result m = co_await db_->execSqlCoro("SELECT author, deleted_ms FROM messages WHERE id = ? AND channel = ?", message, channel);
        if (m.empty())
            co_return fail(404, "no_message");
        if (m[0][1].isNull()) /* deleted first: out of everyone's sight before it is erased */
            co_return fail(409, "not_deleted");
        author = text(m[0][0]);
    } catch (const DrogonDbException &e) {
        co_return server_error(e);
    }
    Answer a = co_await purge_message(member, message);
    if (a.status == 204)
        co_await log_admin(member, "message_purge", author, "message " + std::to_string(message) + " in channel " + std::to_string(channel));
    co_return a;
}

/* ---- pondctl ---- */

Task<Outcome> Service::message_show(int64_t message, std::shared_ptr<std::string> out)
{
    try {
        Result r = co_await db_->execSqlCoro(std::string("SELECT ") + LOG_COLS + " FROM messages WHERE id = ?", message);
        if (r.empty())
            co_return "no message " + std::to_string(message);
        const std::string list = co_await logged_json(db_, r);
        Json::Value v;
        if (!parse_object(list.substr(1, list.size() - 2), v))
            co_return "the message could not be read";
        Result c = co_await db_->execSqlCoro("SELECT name FROM channels WHERE id = ?", v["channel"].asInt64());
        Result who = co_await db_->execSqlCoro("SELECT profile FROM members WHERE id = ?", v["author"].asString());
        std::string name;
        Json::Value profile;
        if (!who.empty() && parse_object(text(who[0][0]), profile))
            name = profile["display_name"].asString();
        std::string &o = *out;
        o += "message " + std::to_string(message) + " in #" + (c.empty() ? "?" : text(c[0][0])) + "\n";
        o += "author:  " + v["author"].asString() + (name.empty() ? "" : " (" + name + ")") + "\n";
        o += "sent:    " + std::to_string(v["ts"].asInt64()) + " ms\n";
        if (v.isMember("edited"))
            o += "edited:  " + std::to_string(v["edited"].asInt64()) + " ms\n";
        if (v.isMember("deleted"))
            o += "deleted: " + std::to_string(v["deleted"]["ts"].asInt64()) + " ms, by " + v["deleted"]["by"].asString() + "\n";
        for (const auto &ver : v["versions"])
            o += "\nuntil " + std::to_string(ver["until"].asInt64()) + " ms it said:\n" + ver["text"].asString() + "\n";
        o += "\n" + std::string(v.isMember("versions") && v["versions"].size() ? "then, last:" : "it says:") + "\n" + v["text"].asString() + "\n";
        for (const auto &file : v["files"]) {
            const std::string id = file["id"].asString();
            o += "\nfile " + file["name"].asString() + " (" + file["mime"].asString() + ", " + std::to_string(file["size"].asInt64()) +
                 " bytes)" + (file.isMember("removed") ? ", dropped by an edit" : "") + "\n  " + file_path(id, false) +
                 (access(file_path(id, false).c_str(), R_OK) == 0 ? "" : " (not here)") + "\n";
        }
        co_return "";
    } catch (const DrogonDbException &e) {
        co_return std::string("database: ") + e.base().what();
    }
}

Task<Outcome> Service::message_delete_ctl(int64_t message)
{
    Answer a = co_await delete_message("pondctl", message);
    if (a.status == 404)
        co_return "no message " + std::to_string(message) + " that is not deleted";
    if (a.status != 204)
        co_return "the database refused";
    co_await log_admin("pondctl", "message_delete", "", "message " + std::to_string(message));
    co_return "";
}

Task<Outcome> Service::message_purge_ctl(int64_t message)
{
    try {
        Result m = co_await db_->execSqlCoro("SELECT deleted_ms FROM messages WHERE id = ?", message);
        if (m.empty())
            co_return "no message " + std::to_string(message);
        if (m[0][0].isNull()) {
            Answer d = co_await delete_message("pondctl", message); /* out of everyone's sight first */
            if (d.status != 204)
                co_return "the database refused";
        }
    } catch (const DrogonDbException &e) {
        co_return std::string("database: ") + e.base().what();
    }
    Answer a = co_await purge_message("pondctl", message);
    if (a.status != 204)
        co_return "the database refused";
    co_await log_admin("pondctl", "message_purge", "", "message " + std::to_string(message));
    co_return "";
}

} // namespace pond
