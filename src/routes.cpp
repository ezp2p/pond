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
constexpr int UNKNOWN = -1; /* the status of an answer that must look like a path that does not exist */

static const Answer NOT_FOUND = {UNKNOWN, ""};

enum Kind { JOIN, KEYS, INFO, HISTORY, SEND, EVENTS, MEMBERS, PROFILE, LEAVE, NONE };

/* Which route `method path` is, and the channel id in it if it has one. */
static Kind route_of(const std::string &method, std::string_view p, int64_t &channel)
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

static bool takes_body(Kind k)
{
    return k == JOIN || k == KEYS || k == SEND || k == PROFILE;
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
    int64_t channel = 0;
    Kind k = route_of(method, path, channel);
    if (k == NONE || (!takes_body(k) && !body.empty()) || body.size() > BODY_MAX)
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
        if (!get_string(json, "text", text) || (json.isMember("nonce") && !get_string(json, "nonce", nonce)) || nonce.size() > 32)
            co_return fail(400, "bad_request");
        for (char c : nonce)
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_'))
                co_return fail(400, "bad_nonce");
        co_return co_await svc_.send(sender, channel, text, nonce);
    }
    case EVENTS: {
        std::map<std::string, int64_t> q;
        if (!query_numbers(req->query(), {"after"}, q))
            co_return fail(400, "bad_query");
        co_return co_await svc_.events(q.count("after") ? q["after"] : 0);
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
            r->setContentTypeCode(drogon::CT_APPLICATION_JSON);
            r->setBody(std::move(a.body));
        } else {
            r->setContentTypeCode(drogon::CT_NONE);
        }
    }
    if (req->getOriginalPath() != "/pond/v1/events") /* a member's Puddle asks for events all day long */
        LOG_INFO << req->peerAddr().toIp() << " " << req->methodString() << " " << req->getOriginalPath() << " -> "
                 << (a.status == UNKNOWN ? 404 : a.status);
    co_return r;
}

} // namespace pond
