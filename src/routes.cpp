#include "routes.h"

#include "base/encoding.h"
#include "base/hash.h"
#include "base/json.h"
#include "core/identity.h"
#include "core/link.h"
#include "core/ratelimit.h"
#include "http/http.h"
#include "http/reqsig.h"
#include "http/router.h"

#include <cstdlib>
#include <cstring>

namespace pond {

constexpr size_t BODY_MAX = 32 * 1024;
constexpr int UNKNOWN = -1; /* the status of an answer that must look like a path that does not exist */

static const Answer NOT_FOUND = {UNKNOWN, ""};

Routes::Routes(Service &s) : svc_(s)
{
    replay_ = puddle_replay_new(0);
    strangers_ = puddle_bucket_new(20, 100);
}

Routes::~Routes()
{
    puddle_replay_free(replay_);
    puddle_bucket_free(strangers_);
}

enum Kind { JOIN, KEYS, INFO, HISTORY, SEND, EVENTS, MEMBERS, PROFILE, LEAVE, NONE };

/* Which route `method path` is, and the channel id in it if it has one. */
static Kind route_of(const char *method, const char *path, std::string &channel)
{
    bool get = !strcmp(method, "GET"), post = !strcmp(method, "POST");
    const char *p = path;
    if (strncmp(p, "/pond/v1/", 9) != 0)
        return NONE;
    p += 9;
    if (post && !strcmp(p, "join")) return JOIN;
    if (post && !strcmp(p, "keys")) return KEYS;
    if (get && !strcmp(p, "info")) return INFO;
    if (get && !strcmp(p, "events")) return EVENTS;
    if (get && !strcmp(p, "members")) return MEMBERS;
    if (post && !strcmp(p, "profile")) return PROFILE;
    if (post && !strcmp(p, "leave")) return LEAVE;
    if (strncmp(p, "channels/", 9) != 0)
        return NONE;
    p += 9;
    const char *slash = strchr(p, '/');
    if (!slash || slash == p || slash - p > 18 || *p == '0')
        return NONE;
    for (const char *c = p; c < slash; c++)
        if (*c < '0' || *c > '9')
            return NONE;
    channel.assign(p, (size_t)(slash - p));
    if (get && !strcmp(slash, "/messages")) return HISTORY;
    if (post && !strcmp(slash, "/send")) return SEND;
    return NONE;
}

static bool takes_body(Kind k)
{
    return k == JOIN || k == KEYS || k == SEND || k == PROFILE;
}

/* A string member of the body, unescaped. */
static bool body_string(const unsigned char *body, size_t len, const char *key, std::string &out, size_t max, bool required = true)
{
    puddle_span v;
    int r = puddle_json_member((const char *)body, len, key, &v);
    if (r != 0) {
        out.clear();
        return r == 1 && !required;
    }
    std::string buf(max + 1, '\0');
    if (puddle_json_string(v, buf.data(), buf.size()) != 0)
        return false;
    out = buf.c_str();
    return true;
}

/* "profile": an object of up to PROFILE_MAX bytes, null, or absent ("" for both). */
static bool body_profile(const unsigned char *body, size_t len, std::string &out)
{
    puddle_span v;
    int r = puddle_json_member((const char *)body, len, "profile", &v);
    out.clear();
    if (r == 1 || (r == 0 && v.len == 4 && !memcmp(v.p, "null", 4)))
        return true;
    if (r != 0 || v.len > PROFILE_MAX || !puddle_json_valid_object(v.p, v.len))
        return false;
    out.assign(v.p, v.len);
    return true;
}

/* The key chain a join or keys request carries, checked. */
static bool body_chain(const unsigned char *body, size_t len, Chain &out)
{
    if (!body_string(body, len, "chain", out.text, 256 * 1024 / 8))
        return false;
    puddle_chain_head head;
    char err[160];
    if (puddle_chain_verify(out.text.data(), out.text.size(), nullptr, &head, err, sizeof err) != 0)
        return false;
    char kid[65], pub[65];
    puddle_key_id(head.sign_pub, kid);
    puddle_hex_encode(head.sign_pub, 32, pub);
    out.id = head.id;
    out.kid = kid;
    out.sign_pub_hex = pub;
    out.seq = head.seq;
    return true;
}

struct Lookup {
    Service *svc;
    const Chain *chain; /* join and keys: the key comes from here */
};

static int lookup_key(void *user, const char *sender, const char *kid, unsigned char pub[32])
{
    auto *l = (Lookup *)user;
    if (l->chain)
        return l->chain->id == sender && l->chain->kid == kid && puddle_hex_decode(l->chain->sign_pub_hex.c_str(), pub, 32) == 0;
    return l->svc->member_key(sender, kid, pub);
}

static int64_t query_num(const char *v, int64_t dflt)
{
    return *v ? strtoll(v, nullptr, 10) : dflt;
}

Answer Routes::handle(const puddle_request &req, const unsigned char *body, size_t body_len)
{
    std::string channel;
    Kind k = route_of(req.method, req.path, channel);
    if (k == NONE || (!takes_body(k) && body_len) || body_len > BODY_MAX)
        return NOT_FOUND;
    int64_t now = clock ? clock() : now_ms();

    Chain chain;
    if (k == JOIN || k == KEYS) {
        /* checking a chain costs signatures, and anyone can send one: refuse floods before the work */
        if ((k == JOIN && !puddle_bucket_take(strangers_, now)) || !body_chain(body, body_len, chain))
            return NOT_FOUND;
    }
    Lookup l = {&svc_, k == JOIN || k == KEYS ? &chain : nullptr};
    puddle_reqsig sig;
    const char *me = k == JOIN ? PUDDLE_RECIPIENT_ANY : svc_.key().id.c_str();
    puddle_authn a = puddle_reqsig_authenticate(req.signature, req.method, req.target, body, body_len, me, lookup_key, &l, replay_,
                                                now, &sig);
    if (a == PUDDLE_AUTHN_REJECT)
        return NOT_FOUND;
    /* from here the sender is who it says, so it may be told what went wrong */
    if (a == PUDDLE_AUTHN_SKEW)
        return {401, "{\"error\":\"clock_skew\",\"now\":" + std::to_string(now) + "}"};
    if (a == PUDDLE_AUTHN_REPLAYED)
        return fail(401, "replayed");
    if (a != PUDDLE_AUTHN_OK)
        return fail(503, "busy");
    std::string sender = sig.id;
    if (k == KEYS && !svc_.is_member(sender))
        return NOT_FOUND;

    static const char *const HISTORY_KEYS[] = {"before", "limit"};
    static const char *const EVENTS_KEYS[] = {"after"};
    char q[2][PUDDLE_QUERY_VALUE_MAX];
    switch (k) {
    case JOIN: {
        std::string invite, profile;
        if (!body_string(body, body_len, "invite", invite, 64) || !puddle_invite_token_valid(invite.c_str()) ||
            !body_profile(body, body_len, profile))
            return fail(400, "bad_request");
        Answer ans = svc_.join(chain, invite, profile);
        return ans.status == 404 ? NOT_FOUND : ans; /* a spent or unknown invite says nothing */
    }
    case KEYS:
        return svc_.keys(sender, chain);
    case INFO:
        return svc_.info(sender);
    case HISTORY: {
        if (puddle_query_parse(req.target, HISTORY_KEYS, 2, q) != 0)
            return fail(400, "bad_query");
        int64_t before = query_num(q[0], 0), limit = query_num(q[1], HISTORY_DEFAULT);
        if (before < 0 || limit < 1 || limit > HISTORY_MAX)
            return fail(400, "bad_query");
        return svc_.history(channel, before, (int)limit);
    }
    case SEND: {
        std::string text, nonce;
        if (!body_string(body, body_len, "text", text, BODY_MAX) || !body_string(body, body_len, "nonce", nonce, 32, false))
            return fail(400, "bad_request");
        for (char c : nonce)
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_'))
                return fail(400, "bad_nonce");
        return svc_.send(sender, channel, text, nonce);
    }
    case EVENTS: {
        if (puddle_query_parse(req.target, EVENTS_KEYS, 1, q) != 0 || query_num(q[0], 0) < 0)
            return fail(400, "bad_query");
        return svc_.events(query_num(q[0], 0));
    }
    case MEMBERS:
        return svc_.members();
    case PROFILE: {
        std::string profile;
        if (!body_profile(body, body_len, profile))
            return fail(400, "bad_profile");
        return svc_.set_profile(sender, profile);
    }
    case LEAVE:
        return svc_.leave(sender);
    case NONE:
        break;
    }
    return NOT_FOUND;
}

/* ---- as a puddle_http_handler ---- */

static int no_owner(void *, const char *)
{
    return 0; /* a Pond has no owner token: its admin uses pondctl */
}

static size_t body_limit(void *, const puddle_request *req, int)
{
    std::string channel;
    return takes_body(route_of(req->method, req->path, channel)) ? BODY_MAX : 0;
}

static size_t route_big(void *user, const puddle_request *req, int, const unsigned char *body, size_t body_len, char *out, size_t cap,
                        int *status, puddle_big *big)
{
    memset(big, 0, sizeof *big);
    Answer a = ((Routes *)user)->handle(*req, body, body_len);
    if (a.status == UNKNOWN) {
        *status = 404;
        return puddle_http_error(404, out, cap);
    }
    *status = a.status;
    if (a.body.empty())
        return puddle_http_response(a.status, "", nullptr, 0, out, cap);
    size_t n = puddle_http_head(a.status, "application/json", "", a.body.size(), out, cap);
    big->data = (unsigned char *)malloc(a.body.size());
    if (!n || !big->data) {
        free(big->data);
        big->data = nullptr;
        *status = 500;
        return puddle_http_error(500, out, cap);
    }
    memcpy(big->data, a.body.data(), a.body.size());
    big->len = a.body.size();
    return n;
}

puddle_http_handler Routes::handler()
{
    puddle_http_handler h = {};
    h.owner_check = no_owner;
    h.body_limit = body_limit;
    h.route_big = route_big;
    h.user = this;
    h.deadline_seconds = 15; /* to read the request; a wait for events is not counted */
    return h;
}

} // namespace pond
