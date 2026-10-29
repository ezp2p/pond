#include "routes.h"

#include "encoding.h"
#include "identity.h"
#include "json.h"

#include <drogon/orm/Exception.h>
#include <trantor/utils/Logger.h>

#include <cstdlib>
#include <cstring>
#include <map>

namespace pond {

using drogon::HttpRequestPtr;
using drogon::HttpResponse;
using drogon::HttpResponsePtr;

constexpr size_t BODY_MAX = 32 * 1024;
constexpr size_t SEND_MAX = 64 * 1024;           /* ten files with previews, and the text */
constexpr size_t BEGIN_MAX = 256 * 1024;         /* the hashes of a file of FILE_HARD_MAX, in base64 */
constexpr int UNKNOWN = -1; /* the status of an answer that must look like a path that does not exist */

static const Answer NOT_FOUND = {UNKNOWN, ""};

enum Kind {
    JOIN, KEYS, INFO, HISTORY, SEND, EVENTS, MEMBERS, PROFILE, LEAVE, FILE_BEGIN, FILE_PUT, FILE_HASHES, FILE_GET, ROLE_ANSWER,
    ADMIN_POND, ADMIN_CHANNEL_CREATE, ADMIN_CHANNEL_EDIT, ADMIN_CHANNEL_DELETE, ADMIN_REMOVE, ADMIN_BAN, ADMIN_BANS, ADMIN_UNBAN, NONE
};

/* The routes only an operator or administrator may use (docs/design.md, "Roles"). */
static bool is_admin(int k)
{
    return k >= ADMIN_POND && k <= ADMIN_UNBAN;
}

/* A user id: 52 of base32. */
static bool user_id_ok(std::string_view s)
{
    if (s.size() != 52)
        return false;
    for (char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= '2' && c <= '7')))
            return false;
    return true;
}

static int64_t number_of(std::string_view s, bool zero);

/* The routes under /pond/v1/admin/: the channel or member in the path, if any. */
static Kind admin_route(bool get, bool post, std::string_view p, int64_t &channel, std::string &member)
{
    if (post && p == "pond") return ADMIN_POND;
    if (post && p == "channels/create") return ADMIN_CHANNEL_CREATE;
    if (get && p == "bans") return ADMIN_BANS;
    auto split = [&](std::string_view prefix, std::string_view &id, std::string_view &verb) {
        if (p.substr(0, prefix.size()) != prefix)
            return false;
        std::string_view rest = p.substr(prefix.size());
        size_t slash = rest.find('/');
        if (slash == std::string_view::npos)
            return false;
        id = rest.substr(0, slash), verb = rest.substr(slash + 1);
        return true;
    };
    std::string_view id, verb;
    if (post && split("channels/", id, verb) && (channel = number_of(id, false)) > 0) {
        if (verb == "edit") return ADMIN_CHANNEL_EDIT;
        if (verb == "delete") return ADMIN_CHANNEL_DELETE;
    }
    if (post && split("members/", id, verb) && user_id_ok(id)) {
        member = std::string(id);
        if (verb == "remove") return ADMIN_REMOVE;
        if (verb == "ban") return ADMIN_BAN;
    }
    if (post && split("bans/", id, verb) && user_id_ok(id) && verb == "unban") {
        member = std::string(id);
        return ADMIN_UNBAN;
    }
    return NONE;
}

/* A whole number of up to 18 digits with no leading zero (or "0" when `zero`), as all of `s`. -1 if it is not one. */
static int64_t number_of(std::string_view s, bool zero)
{
    if (s.empty() || s.size() > 18 || (s[0] == '0' && !(zero && s.size() == 1)))
        return -1;
    for (char c : s)
        if (c < '0' || c > '9')
            return -1;
    return strtoll(std::string(s).c_str(), nullptr, 10);
}

/* The routes under /pond/v1/files/{file}: the file id and the chunk number in it, if any. */
static Kind file_route(bool get, bool post, std::string_view p, std::string &file, int64_t &n)
{
    size_t slash = p.find('/');
    file = std::string(p.substr(0, slash));
    if (file.size() != 64)
        return NONE;
    if (slash == std::string_view::npos)
        return post ? FILE_BEGIN : NONE;
    std::string_view rest = p.substr(slash);
    if (rest == "/chunks")
        return get ? FILE_HASHES : NONE;
    if (rest.substr(0, 8) != "/chunks/" || (n = number_of(rest.substr(8), true)) < 0)
        return NONE;
    return get ? FILE_GET : post ? FILE_PUT : NONE;
}

/* Which route `method path` is, and the channel id, file id and chunk number in it if it has them. */
static Kind route_of(const std::string &method, std::string_view p, int64_t &channel, std::string &file, int64_t &n, std::string &member)
{
    bool get = method == "GET", post = method == "POST";
    if (p.substr(0, 9) != "/pond/v1/")
        return NONE;
    p.remove_prefix(9);
    if (post && p == "join") return JOIN;
    if (post && p == "keys") return KEYS;
    if (get && p == "info") return INFO;
    if (get && p == "events") return EVENTS;
    if (get && p == "members") return MEMBERS;
    if (post && p == "profile") return PROFILE;
    if (post && p == "leave") return LEAVE;
    if (p.substr(0, 6) == "files/")
        return file_route(get, post, p.substr(6), file, n);
    if (post && p == "roles/answer") return ROLE_ANSWER;
    if (p.substr(0, 6) == "admin/")
        return admin_route(get, post, p.substr(6), channel, member);
    if (p.substr(0, 9) != "channels/")
        return NONE;
    p.remove_prefix(9);
    size_t slash = p.find('/');
    if (slash == std::string_view::npos || slash == 0 || slash > 18 || p[0] == '0')
        return NONE;
    for (size_t i = 0; i < slash; i++)
        if (p[i] < '0' || p[i] > '9')
            return NONE;
    channel = strtoll(std::string(p.substr(0, slash)).c_str(), nullptr, 10);
    std::string_view rest = p.substr(slash);
    if (get && rest == "/messages") return HISTORY;
    if (post && rest == "/send") return SEND;
    return NONE;
}

/* Routes whose body is a JSON object. FILE_PUT's is a chunk's bytes. */
static bool takes_body(Kind k)
{
    return k == JOIN || k == KEYS || k == SEND || k == PROFILE || k == FILE_BEGIN || k == ROLE_ANSWER || k == ADMIN_POND ||
           k == ADMIN_CHANNEL_CREATE || k == ADMIN_CHANNEL_EDIT || k == ADMIN_BAN;
}

static size_t body_max(Kind k)
{
    return k == FILE_PUT ? (size_t)FILE_CHUNK : k == FILE_BEGIN ? BEGIN_MAX : k == SEND ? SEND_MAX : BODY_MAX;
}

/* "files": [{id, name, mime, w?, h?, duration_ms?, preview?}], as a send says them; cleaned by the Service. */
static bool body_files(const Json::Value &json, std::vector<FileRef> &out)
{
    if (!json.isMember("files"))
        return true;
    const Json::Value &a = json["files"];
    if (!a.isArray() || a.size() > FILES_PER_MESSAGE)
        return false;
    for (const auto &o : a) {
        FileRef f;
        if (!o.isObject() || !get_string(o, "id", f.id) || !get_string(o, "name", f.name) || !get_string(o, "mime", f.mime))
            return false;
        if (o.isMember("preview") && !get_string(o, "preview", f.preview))
            return false;
        for (auto [key, to] : {std::pair<const char *, int64_t *>{"w", &f.w}, {"h", &f.h}, {"duration_ms", &f.duration_ms}})
            if (o.isMember(key)) {
                if (!o[key].isInt64())
                    return false;
                *to = o[key].asInt64();
            }
        out.push_back(std::move(f));
    }
    return true;
}

/* The query string as `allowed` keys and whole numbers: no other keys, none twice. Missing keys are left out of `out`. */
static bool query_numbers(const std::string &q, std::initializer_list<const char *> allowed, std::map<std::string, int64_t> &out)
{
    size_t p = 0;
    while (p < q.size()) {
        size_t amp = q.find('&', p), end = amp == std::string::npos ? q.size() : amp;
        std::string pair = q.substr(p, end - p);
        p = end + 1;
        size_t eq = pair.find('=');
        if (eq == std::string::npos || eq == 0 || eq + 1 == pair.size() || pair.size() - eq - 1 > 18)
            return false;
        std::string key = pair.substr(0, eq), value = pair.substr(eq + 1);
        bool known = false;
        for (const char *a : allowed)
            known = known || key == a;
        if (!known || out.count(key))
            return false;
        for (char c : value)
            if (c < '0' || c > '9')
                return false;
        out[key] = strtoll(value.c_str(), nullptr, 10);
    }
    return true;
}

/* "profile": an object of up to PROFILE_MAX bytes, kept as sent; null or absent is "". */
static bool body_profile(const std::string &body, const Json::Value &json, std::string &out)
{
    out.clear();
    std::string_view raw;
    if (!json.isMember("profile") || json["profile"].isNull())
        return true;
    if (!json["profile"].isObject() || !raw_member(body, "profile", raw) || raw.size() > PROFILE_MAX)
        return false;
    out = raw;
    return true;
}

/* The key chain a join or keys request carries, checked. */
static bool body_chain(const Json::Value &json, Chain &out)
{
    ChainHead head;
    std::string why;
    if (!get_string(json, "chain", out.text) || !verify_chain(out.text, head, why))
        return false;
    out.id = head.id;
    out.kid = key_id(head.sign_pub);
    out.sign_pub_hex = hex(head.sign_pub, 32);
    out.seq = head.seq;
    return true;
}

Task<Answer> Routes::answer(const HttpRequestPtr &req)
{
    std::string method = req->methodString();
    const std::string &path = req->getOriginalPath();
    std::string target = req->query().empty() ? path : path + "?" + req->query();
    std::string body(req->body());
    int64_t channel = 0, chunk = -1;
    std::string file, member;
    Kind k = route_of(method, path, channel, file, chunk, member);
    if (k == NONE || (!takes_body(k) && k != FILE_PUT && !body.empty()) || body.size() > body_max(k))
        co_return NOT_FOUND;
    int64_t now = now_ms();

    Json::Value json;
    bool json_ok = takes_body(k) && parse_object(body, json);
    Chain chain;
    if (k == JOIN || k == KEYS) {
        /* checking a chain costs signatures, and anyone can send one: refuse floods before the work */
        if (k == JOIN) {
            std::lock_guard<std::mutex> g(strangers_mu_);
            if (!strangers_.take(now))
                co_return NOT_FOUND;
        }
        if (!json_ok || !body_chain(json, chain))
            co_return NOT_FOUND;
    }

    /* whose key the signature must be by: for join and keys the chain's, otherwise the member's as kept */
    std::string header = req->getHeader("x-puddle-signature");
    ReqSig claimed, sig;
    if (!parse_reqsig(header, claimed))
        co_return NOT_FOUND;
    std::array<unsigned char, 32> pub;
    if (k == JOIN || k == KEYS) {
        if (claimed.id != chain.id || claimed.kid != chain.kid || !unhex(chain.sign_pub_hex, pub.data(), 32))
            co_return NOT_FOUND;
    } else {
        auto kept = co_await svc_.member_key(claimed.id, claimed.kid);
        if (!kept)
            co_return NOT_FOUND;
        pub = *kept;
    }
    KeyLookup lookup = [&](const std::string &, const std::string &, unsigned char out[32]) {
        memcpy(out, pub.data(), 32);
        return true;
    };
    std::string me = k == JOIN ? "*" : svc_.key().id;
    switch (authenticate(header, method, target, body, me, lookup, replay_, now, sig)) {
    case Authn::OK:
        break;
    case Authn::REJECT:
        co_return NOT_FOUND;
    /* from here the sender is who it says, so it may be told what went wrong */
    case Authn::SKEW:
        co_return Answer{401, "{\"error\":\"clock_skew\",\"now\":" + std::to_string(now) + "}"};
    case Authn::REPLAYED:
        co_return fail(401, "replayed");
    case Authn::BUSY:
        co_return fail(503, "busy");
    }
    const std::string &sender = sig.id;
    if (takes_body(k) && !json_ok)
        co_return fail(400, "bad_request");
    if (is_admin(k) && co_await svc_.role(sender) == Role::MEMBER)
        co_return NOT_FOUND; /* to a member without a role, these do not exist */

    /* an optional string member: false if it is there and not a string */
    auto opt_string = [&](const char *key, std::optional<std::string> &out) {
        std::string v;
        if (!json.isMember(key))
            return true;
        if (!get_string(json, key, v))
            return false;
        out = v;
        return true;
    };

    switch (k) {
    case JOIN: {
        std::string invite, profile;
        if (!get_string(json, "invite", invite) || !token_ok(invite) || !body_profile(body, json, profile))
            co_return fail(400, "bad_request");
        Answer a = co_await svc_.join(chain, invite, profile);
        co_return a.status == 404 ? NOT_FOUND : a; /* a spent or unknown invite says nothing */
    }
    case KEYS:
        if (!co_await svc_.is_member(sender))
            co_return NOT_FOUND;
        co_return co_await svc_.keys(sender, chain);
    case INFO:
        co_return co_await svc_.info(sender);
    case HISTORY: {
        std::map<std::string, int64_t> q;
        if (!query_numbers(req->query(), {"before", "limit"}, q))
            co_return fail(400, "bad_query");
        int64_t before = q.count("before") ? q["before"] : 0, limit = q.count("limit") ? q["limit"] : HISTORY_DEFAULT;
        if (limit < 1 || limit > HISTORY_MAX)
            co_return fail(400, "bad_query");
        co_return co_await svc_.history(channel, before, (int)limit);
    }
    case SEND: {
        std::string text, nonce;
        std::vector<FileRef> files;
        if ((json.isMember("text") || !json.isMember("files")) && !get_string(json, "text", text))
            co_return fail(400, "bad_request");
        if ((json.isMember("nonce") && !get_string(json, "nonce", nonce)) || nonce.size() > 32)
            co_return fail(400, "bad_request");
        if (!body_files(json, files))
            co_return fail(400, "bad_file");
        for (char c : nonce)
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_'))
                co_return fail(400, "bad_nonce");
        co_return co_await svc_.send(sender, channel, text, nonce, std::move(files));
    }
    case EVENTS: {
        std::map<std::string, int64_t> q;
        if (!query_numbers(req->query(), {"after"}, q))
            co_return fail(400, "bad_query");
        co_return co_await svc_.events(q.count("after") ? q["after"] : 0, sender);
    }
    case MEMBERS:
        co_return co_await svc_.members();
    case PROFILE: {
        std::string profile;
        if (!body_profile(body, json, profile))
            co_return fail(400, "bad_profile");
        co_return co_await svc_.set_profile(sender, profile);
    }
    case LEAVE:
        co_return co_await svc_.leave(sender);
    case FILE_BEGIN: {
        std::string chunks;
        if (!json.isMember("size") || !json["size"].isInt64() || !get_string(json, "chunks", chunks) || chunks.size() % 4 != 0)
            co_return fail(400, "bad_request");
        std::string hashes(chunks.size() / 4 * 3, '\0');
        size_t pad = chunks.empty() ? 0 : chunks.back() == '=' ? (chunks[chunks.size() - 2] == '=' ? 2 : 1) : 0;
        hashes.resize(hashes.size() - pad);
        if (hashes.empty() || !unbase64(chunks, (unsigned char *)hashes.data(), hashes.size()))
            co_return fail(400, "bad_request");
        co_return co_await svc_.file_begin(sender, file, json["size"].asInt64(), std::move(hashes));
    }
    case FILE_PUT:
        co_return co_await svc_.file_put(sender, file, chunk, std::move(body));
    case FILE_HASHES:
        co_return co_await svc_.file_chunks(sender, file);
    case FILE_GET:
        co_return co_await svc_.file_chunk(sender, file, chunk);
    case ROLE_ANSWER:
        if (!json.isMember("offer") || !json["offer"].isInt64() || !json.isMember("accept") || !json["accept"].isBool())
            co_return fail(400, "bad_request");
        co_return co_await svc_.answer_offer(sender, json["offer"].asInt64(), json["accept"].asBool());
    case ADMIN_POND: {
        std::optional<std::string> name, description;
        if (!opt_string("name", name) || !opt_string("description", description))
            co_return fail(400, "bad_request");
        co_return co_await svc_.admin_pond(sender, name, description);
    }
    case ADMIN_CHANNEL_CREATE: {
        std::optional<std::string> name, topic;
        if (!opt_string("name", name) || !name || !opt_string("topic", topic))
            co_return fail(400, "bad_request");
        co_return co_await svc_.admin_channel_create(sender, *name, topic.value_or(""));
    }
    case ADMIN_CHANNEL_EDIT: {
        std::optional<std::string> name, topic;
        if (!opt_string("name", name) || !opt_string("topic", topic))
            co_return fail(400, "bad_request");
        co_return co_await svc_.admin_channel_edit(sender, channel, name, topic);
    }
    case ADMIN_CHANNEL_DELETE:
        co_return co_await svc_.admin_channel_delete(sender, channel);
    case ADMIN_REMOVE:
        co_return co_await svc_.admin_remove(sender, member, false, "");
    case ADMIN_BAN: {
        std::optional<std::string> reason;
        if (!opt_string("reason", reason))
            co_return fail(400, "bad_request");
        co_return co_await svc_.admin_remove(sender, member, true, reason.value_or(""));
    }
    case ADMIN_BANS:
        co_return co_await svc_.admin_bans();
    case ADMIN_UNBAN:
        co_return co_await svc_.admin_unban(sender, member);
    case NONE:
        break;
    }
    co_return NOT_FOUND;
}

HttpResponsePtr Routes::not_found()
{
    auto r = HttpResponse::newHttpResponse();
    r->setStatusCode(drogon::k404NotFound);
    r->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    r->setBody("{\"error\":\"not_found\"}");
    return r;
}

Task<HttpResponsePtr> Routes::handle(HttpRequestPtr req)
{
    Answer a;
    try {
        a = co_await answer(req);
    } catch (const drogon::orm::DrogonDbException &e) {
        LOG_ERROR << "database: " << e.base().what();
        a = fail(500, "database");
    } catch (const std::exception &e) {
        LOG_ERROR << req->methodString() << " " << req->getOriginalPath() << ": " << e.what();
        a = fail(500, "internal");
    }
    HttpResponsePtr r;
    if (a.status == UNKNOWN) {
        r = not_found();
    } else {
        r = HttpResponse::newHttpResponse();
        r->setStatusCode((drogon::HttpStatusCode)a.status);
        if (!a.body.empty()) {
            r->setContentTypeCode(a.bytes ? drogon::CT_APPLICATION_OCTET_STREAM : drogon::CT_APPLICATION_JSON);
            r->setBody(std::move(a.body));
        } else {
            r->setContentTypeCode(drogon::CT_NONE);
        }
    }
    /* a member's Puddle asks for events all day long, and files come a chunk at a time */
    if (req->getOriginalPath() != "/pond/v1/events" && req->getOriginalPath().find("/chunks/") == std::string::npos)
        LOG_INFO << req->peerAddr().toIp() << " " << req->methodString() << " " << req->getOriginalPath() << " -> "
                 << (a.status == UNKNOWN ? 404 : a.status);
    co_return r;
}

} // namespace pond
