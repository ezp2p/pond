/* The entry form (docs/design.md, "Entry form"): a Pond whose form page says `required_to_join` asks those joining to answer it
 * first. Joining with a good invite then answers 409 `application_required` with the form; the Puddle applies with the
 * answers (POST /pond/v1/apply, as a stranger, as a join is), spending one use of the invite; whoever may manage applications
 * accepts (they are a member at once) or declines, with a reason. Meanwhile joining answers `application_pending`, and after a
 * refusal `application_declined` with the reason; once accepted, joining works as for anyone. */

#include "pond.h"

#include "json.h"
#include "text.h"

#include <drogon/orm/Exception.h>
#include <trantor/utils/Logger.h>

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

Task<std::string> Service::entry_form()
{
    Result r = co_await db_->execSqlCoro("SELECT id, body, COALESCE(data, '{}') FROM pages WHERE kind = 'form' ORDER BY position, id");
    for (const auto &row : r) {
        Json::Value d;
        if (!parse_object(text(row[2]), d) || !d.get("required_to_join", false).asBool())
            continue;
        Json::StreamWriterBuilder b;
        b["indentation"] = "";
        b["emitUTF8"] = true;
        co_return "{\"page\":" + std::to_string(num(row[0])) + ",\"intro\":" + jstr(text(row[1])) + ",\"questions\":" +
            Json::writeString(b, d["questions"]) + "}";
    }
    co_return "";
}

/* The answers checked against the form, as [{id, label, answer}]: "" if they fit, else why not */
static std::string check_answers(const Json::Value &form, const Json::Value &given, std::string &out)
{
    Json::Value list(Json::arrayValue);
    std::set<std::string> asked;
    for (const auto &q : form["questions"]) {
        const std::string id = q["id"].asString(), kind = q["kind"].asString(), label = q["label"].asString();
        asked.insert(id);
        const Json::Value &a = given.isMember(id) ? given[id] : Json::Value();
        Json::Value e;
        e["id"] = id, e["label"] = label;
        std::set<std::string> choices;
        for (const auto &c : q["choices"])
            choices.insert(c.asString());
        if (kind == "checks") {
            if (!a.isNull() && !a.isArray())
                return "“" + label + "” is answered with a list of its choices";
            Json::Value picked(Json::arrayValue);
            for (const auto &c : a) {
                if (!c.isString() || !choices.count(c.asString()))
                    return "“" + label + "” has no such choice";
                picked.append(c.asString());
            }
            if (q["required"].asBool() && picked.empty())
                return "“" + label + "” needs an answer";
            e["answer"] = picked;
        } else {
            std::string t;
            if (!a.isNull() && (!a.isString() || !clean_text(a.asString(), kind == "long" ? FORM_ANSWER_MAX : FORM_TEXT_MAX, kind == "long", t)))
                return "“" + label + "” is too long";
            if (kind == "choice" && !t.empty() && !choices.count(t))
                return "“" + label + "” has no such choice";
            if (q["required"].asBool() && t.empty())
                return "“" + label + "” needs an answer";
            e["answer"] = t;
        }
        list.append(e);
    }
    for (const auto &k : given.getMemberNames())
        if (!asked.count(k))
            return "the form has no question " + k;
    Json::StreamWriterBuilder b;
    b["indentation"] = "";
    b["emitUTF8"] = true;
    out = Json::writeString(b, list);
    return "";
}

Task<Answer> Service::apply(Chain chain, std::string invite, std::string profile, std::string answers)
{
    TxPtr tx;
    try {
        const std::string form = co_await entry_form();
        Json::Value f, given;
        if (form.empty() || !parse_object(form, f))
            co_return fail(409, "no_form"); /* nothing to apply for: join */
        if (!parse_object(answers, given))
            co_return fail(400, "bad_request");
        std::string checked;
        const std::string why = check_answers(f, given, checked);
        if (!why.empty())
            co_return bad(why);
        tx = co_await db_->newTransactionCoro();
        co_await tx->execSqlCoro("SELECT 1 FROM invites WHERE token = ? FOR UPDATE", invite);
        Result banned = co_await tx->execSqlCoro("SELECT 1 FROM bans WHERE member = ?", chain.id);
        if (!banned.empty()) {
            tx->rollback();
            co_return fail(404, "not_found");
        }
        Result m = co_await tx->execSqlCoro("SELECT 1 FROM members WHERE id = ? AND left_ms IS NULL", chain.id);
        if (!m.empty()) {
            tx->rollback();
            co_return fail(409, "already_member");
        }
        Result had = co_await tx->execSqlCoro("SELECT status FROM applications WHERE member = ? FOR UPDATE", chain.id);
        if (!had.empty() && text(had[0][0]) == "pending") {
            tx->rollback();
            co_return fail(409, "application_pending");
        }
        const int64_t now = now_ms();
        Result u = co_await tx->execSqlCoro("UPDATE invites SET uses = uses + 1 WHERE token = ? AND revoked = 0"
                                            " AND (max_uses IS NULL OR uses < max_uses) AND (expires_ms IS NULL OR expires_ms > ?)",
                                            invite, now);
        if (u.affectedRows() != 1) {
            tx->rollback();
            co_return fail(404, "not_found");
        }
        const std::optional<std::string> shown = profile.empty() ? std::nullopt : std::optional<std::string>(profile);
        co_await tx->execSqlCoro("REPLACE INTO applications (member, sign_pub, kid, seq, chain, profile, invite, answers, status, created_ms)"
                                 " VALUES (?, ?, ?, ?, ?, ?, ?, ?, 'pending', ?)",
                                 chain.id, chain.sign_pub_hex, chain.kid, (int64_t)chain.seq, chain.text, shown, invite, checked, now);
        const int64_t ev = co_await add_event(tx, "applications", "{}");
        if (!co_await Committed(std::move(tx)))
            co_return fail(500, "database");
        bump(ev);
        LOG_INFO << chain.id << " applied to join, with invite " << invite.substr(0, 6) << "...";
        co_return Answer{200, "{\"status\":\"pending\"}"};
    } catch (const DrogonDbException &e) {
        if (tx)
            tx->rollback();
        co_return server_error(e);
    }
}

Task<Answer> Service::admin_applications()
{
    try {
        Result r = co_await db_->execSqlCoro("SELECT member, profile, answers, status, reason, created_ms, decided_by, decided_ms FROM applications"
                                             " ORDER BY status = 'pending' DESC, created_ms DESC LIMIT 200");
        std::string out = "{\"applications\":[";
        for (size_t i = 0; i < r.size(); i++)
            out += std::string(i ? "," : "") + "{\"member\":" + jstr(text(r[i][0])) + ",\"profile\":" + (r[i][1].isNull() ? std::string("null") : text(r[i][1])) +
                   ",\"answers\":" + text(r[i][2]) + ",\"status\":" + jstr(text(r[i][3])) + ",\"reason\":" + jstr(text(r[i][4])) +
                   ",\"created\":" + std::to_string(num(r[i][5])) + ",\"decided_by\":" + (r[i][6].isNull() ? std::string("null") : jstr(text(r[i][6]))) +
                   ",\"decided\":" + (r[i][7].isNull() ? std::string("null") : std::to_string(num(r[i][7]))) + "}";
        co_return Answer{200, out + "]}"};
    } catch (const DrogonDbException &e) {
        co_return server_error(e);
    }
}

Task<Answer> Service::admin_application(std::string by, std::string member, bool accept, std::string reason)
{
    std::string why;
    if (!clean_text(reason, BAN_REASON_MAX, false, why))
        co_return bad("a reason is one line of up to 500 characters");
    TxPtr tx;
    try {
        tx = co_await db_->newTransactionCoro();
        Result r = co_await tx->execSqlCoro("SELECT sign_pub, kid, seq, chain, profile FROM applications WHERE member = ? AND status = 'pending' FOR UPDATE",
                                            member);
        if (r.empty()) {
            tx->rollback();
            co_return fail(404, "no_application");
        }
        const int64_t now = now_ms();
        const std::string status = accept ? "accepted" : "declined";
        co_await tx->execSqlCoro("UPDATE applications SET status = ?, reason = ?, decided_by = ?, decided_ms = ? WHERE member = ?", status, why, by, now,
                                 member);
        if (accept) {
            const std::optional<std::string> shown = r[0][4].isNull() ? std::nullopt : std::optional<std::string>(text(r[0][4]));
            co_await tx->execSqlCoro("INSERT INTO members (id, sign_pub, kid, seq, chain, profile, joined_ms) VALUES (?, ?, ?, ?, ?, ?, ?)"
                                     " ON DUPLICATE KEY UPDATE sign_pub = VALUES(sign_pub), kid = VALUES(kid), seq = VALUES(seq),"
                                     " chain = VALUES(chain), profile = VALUES(profile), joined_ms = VALUES(joined_ms), left_ms = NULL",
                                     member, text(r[0][0]), text(r[0][1]), num(r[0][2]), text(r[0][3]), shown, now);
            const std::string mj = co_await member_json_of(tx, member);
            co_await add_event(tx, "member", mj);
        }
        const int64_t ev = co_await add_event(tx, "applications", "{}");
        if (!co_await Committed(std::move(tx)))
            co_return fail(500, "database");
        bump(ev);
    } catch (const DrogonDbException &e) {
        if (tx)
            tx->rollback();
        co_return server_error(e);
    }
    const std::string action = accept ? "application_accept" : "application_decline";
    co_await log_admin(by, action, member, why);
    co_return Answer{204, ""};
}

} // namespace pond
