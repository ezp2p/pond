#include "crash.h"
#include "routes.h"

#include "encoding.h"
#include "identity.h"
#include "json.h"
#include "seal.h"

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
constexpr size_t PAGE_EDIT_MAX = 512 * 1024;      /* a page: its text (up to 4 bytes a character, escaped), and its files */
constexpr size_t SEND_MAX = 64 * 1024;           /* ten files with previews, and the text */
constexpr size_t BEGIN_MAX = 256 * 1024;         /* the hashes of a file of FILE_HARD_MAX, in base64 */
constexpr size_t VOICE_JOIN_MAX = 64 * 1024;     /* an offer: eighteen media sections, each with its candidates */
constexpr size_t DM_SEND_MAX = DM_SEALED_MAX + 1024;
constexpr int UNKNOWN = -1; /* the status of an answer that must look like a path that does not exist */

static const Answer NOT_FOUND = {UNKNOWN, ""};

enum Kind {
    JOIN, KEYS, INFO, HISTORY, SEND, EVENTS, MEMBERS, PROFILE, LEAVE, FILE_BEGIN, FILE_PUT, FILE_HASHES, FILE_GET, ROLE_ANSWER,
    VOICE_JOIN, VOICE_STATE, VOICE_WATCH, VOICE_LEAVE, PICTURE_PUT, PICTURE_GET, MEMBER_CHAIN, DM_SEND, DM_SETTINGS, DM_PIECE_GET,
    DM_PIECE_PUT, PAGE_GET, ROLE_COLOR,
    ADMIN_POND, ADMIN_ICON, ADMIN_CHANNEL_CREATE, ADMIN_CHANNEL_EDIT, ADMIN_CHANNEL_DELETE, ADMIN_REMOVE, ADMIN_BAN, ADMIN_BANS, ADMIN_UNBAN,
    ADMIN_INVITES, ADMIN_INVITE_CREATE, ADMIN_INVITE_REVOKE, ADMIN_MUTE, ADMIN_ROLE_CREATE, ADMIN_ROLE_EDIT, ADMIN_ROLE_DELETE,
    ADMIN_MEMBER_ROLES, ADMIN_CHANNEL_ROLES, ADMIN_CHANNEL_ORDER, ADMIN_ROLE_ORDER, ADMIN_BANNER, ADMIN_PAGE_CREATE,
    ADMIN_PAGE_EDIT, ADMIN_PAGE_DELETE, ADMIN_PAGE_ORDER, ADMIN_ROLE_COLORS, ADMIN_UNMUTE, NONE
};

/* The routes only an operator or administrator may use (docs/design.md, "Roles"), or a member whose roles let them. */
static bool is_admin(int k)
{
    return k >= ADMIN_POND && k <= ADMIN_UNMUTE;
}

/* The permission an admin route needs (docs/design.md, "Permissions") */
static int64_t permission_for(int k)
{
    switch (k) {
    case ADMIN_POND:
    case ADMIN_ICON:
    case ADMIN_BANNER:
    case ADMIN_ROLE_COLORS:
        return MANAGE_POND;
    case ADMIN_PAGE_CREATE:
    case ADMIN_PAGE_EDIT:
    case ADMIN_PAGE_DELETE:
    case ADMIN_PAGE_ORDER:
        return MANAGE_PAGES;
    case ADMIN_CHANNEL_CREATE:
    case ADMIN_CHANNEL_EDIT:
    case ADMIN_CHANNEL_DELETE:
    case ADMIN_CHANNEL_ROLES:
    case ADMIN_CHANNEL_ORDER:
        return MANAGE_CHANNELS;
    case ADMIN_ROLE_CREATE:
    case ADMIN_ROLE_EDIT:
    case ADMIN_ROLE_DELETE:
    case ADMIN_ROLE_ORDER:
    case ADMIN_MEMBER_ROLES:
        return MANAGE_ROLES;
    case ADMIN_INVITES:
    case ADMIN_INVITE_CREATE:
    case ADMIN_INVITE_REVOKE:
        return MANAGE_INVITES;
    case ADMIN_REMOVE:
        return KICK_MEMBERS;
    case ADMIN_BAN:
    case ADMIN_BANS:
    case ADMIN_UNBAN:
        return BAN_MEMBERS;
    case ADMIN_MUTE:
    case ADMIN_UNMUTE:
        return MUTE_MEMBERS;
    }
    return -1; /* none has every bit: only a rank */
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

/* 64 of lower-case hex: a letter's id */
static bool hex_id_ok(std::string_view s)
{
    if (s.size() != 64)
        return false;
    for (char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
    return true;
}

/* The routes under /pond/v1/admin/: the channel or member in the path, if any. */
static Kind admin_route(bool get, bool post, std::string_view p, int64_t &channel, std::string &member)
{
    if (post && p == "pond") return ADMIN_POND;
    if (post && p == "icon") return ADMIN_ICON;
    if (post && p == "banner") return ADMIN_BANNER;
    if (post && p == "role_colors") return ADMIN_ROLE_COLORS;
    if (post && p == "pages/create") return ADMIN_PAGE_CREATE;
    if (post && p == "pages/order") return ADMIN_PAGE_ORDER;
    if (post && p == "channels/create") return ADMIN_CHANNEL_CREATE;
    if (post && p == "channels/order") return ADMIN_CHANNEL_ORDER;
    if (post && p == "roles/create") return ADMIN_ROLE_CREATE;
    if (post && p == "roles/order") return ADMIN_ROLE_ORDER;
    if (get && p == "bans") return ADMIN_BANS;
    if (get && p == "invites") return ADMIN_INVITES;
    if (post && p == "invites/create") return ADMIN_INVITE_CREATE;
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
        if (verb == "roles") return ADMIN_CHANNEL_ROLES;
    }
    if (post && split("pages/", id, verb) && (channel = number_of(id, false)) > 0) { /* the page's id, in `channel` */
        if (verb == "edit") return ADMIN_PAGE_EDIT;
        if (verb == "delete") return ADMIN_PAGE_DELETE;
    }
    if (post && split("roles/", id, verb) && (channel = number_of(id, false)) > 0) { /* the role's id, in `channel` */
        if (verb == "edit") return ADMIN_ROLE_EDIT;
        if (verb == "delete") return ADMIN_ROLE_DELETE;
    }
    if (post && split("members/", id, verb) && user_id_ok(id)) {
        member = std::string(id);
        if (verb == "remove") return ADMIN_REMOVE;
        if (verb == "ban") return ADMIN_BAN;
        if (verb == "mute") return ADMIN_MUTE;
        if (verb == "unmute") return ADMIN_UNMUTE;
        if (verb == "roles") return ADMIN_MEMBER_ROLES;
    }
    if (post && split("bans/", id, verb) && user_id_ok(id) && verb == "unban") {
        member = std::string(id);
        return ADMIN_UNBAN;
    }
    if (post && split("invites/", id, verb) && token_ok(std::string(id)) && verb == "revoke") {
        member = std::string(id); /* the invite's token */
        return ADMIN_INVITE_REVOKE;
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

/* Which route `method path` is, and the channel id, file id (or voice session) and chunk number in it if it has them. */
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
    if (post && p == "role_color") return ROLE_COLOR;
    if (get && p.substr(0, 6) == "pages/" && (channel = number_of(p.substr(6), false)) > 0) return PAGE_GET; /* the page's id, in `channel` */
    if (get && p.substr(0, 8) == "members/" && p.size() == 8 + 52 + 6 && p.substr(8 + 52) == "/chain" && user_id_ok(p.substr(8, 52))) {
        member = std::string(p.substr(8, 52));
        return MEMBER_CHAIN;
    }
    if (post && p == "dms/send") return DM_SEND;
    if (post && p == "dms/settings") return DM_SETTINGS;
    if (p.substr(0, 10) == "dms/files/") {
        /* dms/files/{letter}/{n}/{part}: the letter's id (64 hex), the file's place in it, and "hashes" or a chunk's number */
        std::string_view rest = p.substr(10);
        if (rest.size() < 64 + 4 || rest[64] != '/' || !hex_id_ok(rest.substr(0, 64)))
            return NONE;
        file = std::string(rest.substr(0, 64));
        rest.remove_prefix(65);
        size_t slash = rest.find('/');
        if (slash != 1 || rest[0] < '0' || rest[0] > '9')
            return NONE;
        channel = rest[0] - '0'; /* the file's place, 0 to 9 */
        member = std::string(rest.substr(2)); /* the part, checked by the Service */
        return get ? DM_PIECE_GET : post ? DM_PIECE_PUT : NONE;
    }
    if (post && p == "pictures") return PICTURE_PUT;
    if (get && p.substr(0, 9) == "pictures/" && picture_hash_ok(std::string(p.substr(9)))) {
        file = std::string(p.substr(9)); /* the picture's hash */
        return PICTURE_GET;
    }
    if (post && p.substr(0, 6) == "voice/" && p.size() > 6 + 26 && p[6 + 26] == '/' && token_ok(std::string(p.substr(6, 26)))) {
        file = std::string(p.substr(6, 26)); /* the session */
        std::string_view verb = p.substr(6 + 27);
        if (verb == "state") return VOICE_STATE;
        if (verb == "watch") return VOICE_WATCH;
        if (verb == "leave") return VOICE_LEAVE;
        return NONE;
    }
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
    if (post && rest == "/voice/join") return VOICE_JOIN;
    return NONE;
}

/* Routes whose body is a JSON object. FILE_PUT's is a chunk's bytes, and PICTURE_PUT's a picture's. */
static bool takes_body(Kind k)
{
    return k == JOIN || k == KEYS || k == SEND || k == PROFILE || k == DM_SEND || k == DM_SETTINGS || k == FILE_BEGIN || k == ROLE_ANSWER || k == ADMIN_POND || k == ADMIN_ICON || k == ADMIN_INVITE_CREATE || k == ADMIN_MUTE ||
           k == ADMIN_ROLE_CREATE || k == ADMIN_ROLE_EDIT || k == ADMIN_MEMBER_ROLES || k == ADMIN_CHANNEL_ROLES || k == ADMIN_CHANNEL_ORDER || k == ADMIN_ROLE_ORDER || k == ADMIN_BANNER || k == ADMIN_PAGE_CREATE || k == ADMIN_PAGE_EDIT || k == ADMIN_PAGE_ORDER || k == ADMIN_ROLE_COLORS || k == ROLE_COLOR ||
           k == ADMIN_CHANNEL_CREATE || k == ADMIN_CHANNEL_EDIT || k == ADMIN_BAN || k == VOICE_JOIN || k == VOICE_STATE ||
           k == VOICE_WATCH;
}

static size_t body_max(Kind k)
{
    return k == FILE_PUT ? (size_t)FILE_CHUNK : k == DM_PIECE_PUT ? DM_PIECE_MAX : k == DM_SEND ? DM_SEND_MAX : k == PICTURE_PUT ? PICTURE_MAX : k == FILE_BEGIN ? BEGIN_MAX : k == SEND ? SEND_MAX : k == VOICE_JOIN ? VOICE_JOIN_MAX : (k == ADMIN_PAGE_CREATE || k == ADMIN_PAGE_EDIT) ? PAGE_EDIT_MAX : BODY_MAX;
}

/* "files": [{id, name, mime, w?, h?, duration_ms?, preview?}], as a send says them; cleaned by the Service. */
static bool body_files(const Json::Value &json, std::vector<FileRef> &out, size_t max = FILES_PER_MESSAGE)
{
    if (!json.isMember("files"))
        return true;
    const Json::Value &a = json["files"];
    if (!a.isArray() || a.size() > max)
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

/* `json[key]`: an array of up to `max` ids (whole numbers above 0) */
static bool body_ids(const Json::Value &json, const char *key, size_t max, std::vector<int64_t> &out)
{
    const Json::Value &a = json[key];
    if (!a.isArray() || a.size() > max)
        return false;
    for (const auto &v : a) {
        if (!v.isInt64() || v.asInt64() < 1)
            return false;
        out.push_back(v.asInt64());
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

Task<Answer> Routes::answer(HttpRequestPtr req, Signing &signing)
{
    std::string method = req->methodString();
    const std::string &path = req->getOriginalPath();
    std::string target = req->query().empty() ? path : path + "?" + req->query();
    std::string sent(req->body()); /* as it came: sealed (spec/09), and what the signature covers */
    int64_t channel = 0, chunk = -1;
    std::string file, member;
    Kind k = route_of(method, path, channel, file, chunk, member);
    if ((k == VOICE_JOIN || k == VOICE_STATE || k == VOICE_WATCH || k == VOICE_LEAVE) && !voice_)
        k = NONE; /* voice is off: there are no such paths */
    bool has_seal = !req->getHeader("x-puddle-seal").empty();
    size_t limit = body_max(k) + (has_seal && body_max(k) ? SEAL_TAG : 0);
    if (k == NONE || (!takes_body(k) && k != FILE_PUT && k != PICTURE_PUT && k != DM_PIECE_PUT && !sent.empty() && !has_seal) || sent.size() > limit)
        co_return NOT_FOUND;
    int64_t now = now_ms();

    /* Whoever it says it is from: the seal is opened with that (a join's chain is inside it), the signature checked after */
    std::string header = req->getHeader("x-puddle-signature");
    ReqSig claimed, sig;
    if (!parse_reqsig(header, claimed))
        co_return NOT_FOUND;
    std::string me = k == JOIN ? "*" : svc_.key().id;
    std::string body; /* opened: what the request says */
    if (has_seal) {
        std::string kid;
        unsigned char enc[32];
        Hpke hc;
        if (!parse_seal_header(req->getHeader("x-puddle-seal"), kid, enc) || kid != sha256_hex(std::string_view((const char *)svc_.key().seal_pub, 32)) ||
            !hpke_receiver(enc, svc_.key().seal_secret.get(), seal_info(claimed.id, me, claimed.nonce), hc) ||
            (!sent.empty() && !hpke_open(hc, seal_aad(method, target), sent, body)) || !answer_key(hc, signing.key))
            co_return NOT_FOUND; /* not sealed to us, for this request: a stranger learns nothing */
        signing.sealed = true;
        if ((!takes_body(k) && k != FILE_PUT && k != PICTURE_PUT && k != DM_PIECE_PUT && !body.empty()) || body.size() > body_max(k))
            co_return NOT_FOUND;
    }

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
    Authn authn = authenticate(header, method, target, sent, me, lookup, replay_, now, sig);
    if (authn != Authn::REJECT) { /* who it says: answers to them are signed */
        signing.nonce = sig.nonce;
        signing.only_ok = k == JOIN;
    }
    switch (authn) {
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
    if (!signing.sealed) /* only sealed requests: a Pond's invites carry its key, so there is no first contact without one */
        co_return fail(400, "not_sealed");
    const std::string &sender = sig.id;
    if (takes_body(k) && !json_ok)
        co_return fail(400, "bad_request");
    if (is_admin(k)) {
        const Powers powers = co_await svc_.powers(sender);
        if (!powers.has(permission_for(k)))
            co_return NOT_FOUND; /* to a member not let use them, these do not exist */
    }

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
        if (a.status == 404)
            co_return NOT_FOUND; /* a spent or unknown invite says nothing */
        co_return a; /* if, not ?: (docs/design.md, "Coroutines") */
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
        co_return co_await svc_.history(sender, channel, before, (int)limit);
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
        const int64_t after = q.count("after") ? q["after"] : 0;
        co_return co_await svc_.events(after, sender);
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
    case PICTURE_PUT:
        co_return co_await svc_.picture_put(sender, std::move(body));
    case PICTURE_GET:
        co_return co_await svc_.picture_get(sender, file);
    case MEMBER_CHAIN:
        co_return co_await svc_.member_chain(member);
    case DM_SEND: {
        std::string to, id, kid, enc, sealed;
        if (!get_string(json, "to", to) || !user_id_ok(to) || !get_string(json, "id", id) || !hex_id_ok(id) || !get_string(json, "kid", kid) ||
            !hex_id_ok(kid) || !get_string(json, "enc", enc) || enc.size() != 44 || !get_string(json, "sealed", sealed) || sealed.empty() ||
            sealed.size() > DM_SEALED_MAX)
            co_return fail(400, "bad_request");
        co_return co_await svc_.dm_send(sender, to, id, kid, enc, sealed);
    }
    case DM_SETTINGS:
        if (!json.isMember("accept") || !json["accept"].isBool())
            co_return fail(400, "bad_request");
        co_return co_await svc_.dm_accept(sender, json["accept"].asBool());
    case DM_PIECE_GET:
    case DM_PIECE_PUT: {
        /* ?from= the sender (asking) or ?to= the recipient (sending) */
        const std::string q = req->query(), want = k == DM_PIECE_GET ? "from=" : "to=";
        if (q.size() != want.size() + 52 || q.compare(0, want.size(), want) != 0 || !user_id_ok(std::string_view(q).substr(want.size())))
            co_return fail(400, "bad_query");
        const std::string other = q.substr(want.size());
        if (k == DM_PIECE_GET)
            co_return co_await svc_.dm_piece_get(sender, other, file, (int)channel, member);
        co_return co_await svc_.dm_piece_put(sender, other, file, (int)channel, member, std::move(body));
    }
    case ROLE_COLOR:
        if (!json.isMember("use") || !json["use"].isBool())
            co_return fail(400, "bad_request");
        co_return co_await svc_.role_color(sender, json["use"].asBool());
    case ADMIN_ROLE_COLORS:
        if (!json.isMember("enforced") || !json["enforced"].isBool())
            co_return fail(400, "bad_request");
        co_return co_await svc_.admin_role_colors(sender, json["enforced"].asBool());
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
    case ADMIN_ICON:
    case ADMIN_BANNER: {
        /* {"hash": a picture sent first (POST /pond/v1/pictures)} or {"hash": null} for none */
        std::optional<std::string> hash;
        if (!json.isMember("hash") || (!json["hash"].isNull() && !opt_string("hash", hash)))
            co_return fail(400, "bad_request");
        if (k == ADMIN_BANNER)
            co_return co_await svc_.admin_icon(sender, hash, "banner");
        co_return co_await svc_.admin_icon(sender, hash);
    }
    case PAGE_GET:
        co_return co_await svc_.page_get(sender, channel);
    case ADMIN_PAGE_CREATE:
    case ADMIN_PAGE_EDIT: {
        /* {title?, kind?, body?, files?}: a new page needs its title; files as a message's, sent first */
        PageEdit e;
        if (!opt_string("title", e.title) || !opt_string("kind", e.kind) || !opt_string("body", e.body))
            co_return fail(400, "bad_request");
        if (json.isMember("files")) {
            std::vector<FileRef> files;
            if (!json["files"].isArray() || json["files"].size() > PAGE_FILES_MAX || !body_files(json, files, PAGE_FILES_MAX))
                co_return fail(400, "bad_file");
            e.files = std::move(files);
        }
        if (k == ADMIN_PAGE_CREATE)
            co_return co_await svc_.admin_page_create(sender, std::move(e));
        co_return co_await svc_.admin_page_edit(sender, channel, std::move(e));
    }
    case ADMIN_PAGE_DELETE:
        co_return co_await svc_.admin_page_delete(sender, channel);
    case ADMIN_PAGE_ORDER: {
        /* {pages: [ids]}: every page, first to last */
        std::vector<int64_t> pages;
        if (!body_ids(json, "pages", (size_t)PAGES_MAX, pages))
            co_return fail(400, "bad_request");
        co_return co_await svc_.admin_page_order(sender, std::move(pages));
    }
    case ADMIN_CHANNEL_CREATE: {
        std::optional<std::string> name, topic, kind;
        if (!opt_string("name", name) || !name || !opt_string("topic", topic) || !opt_string("kind", kind))
            co_return fail(400, "bad_request");
        co_return co_await svc_.admin_channel_create(sender, *name, topic.value_or(""), kind.value_or("text"));
    }
    case VOICE_JOIN: {
        std::string sdp;
        if (!get_string(json, "sdp", sdp) || sdp.empty() || !json.isMember("udp") || !json["udp"].isInt() || json["udp"].asInt() < 0 ||
            json["udp"].asInt() > 65535)
            co_return fail(400, "bad_request");
        const int udp = json["udp"].asInt();
        const std::string ip = req->peerAddr().toIp();
        Answer joined = co_await voice_->join(sender, channel, sdp, udp, ip);
        co_return joined;
    }
    case VOICE_STATE: {
        std::optional<bool> st[4];
        const char *keys[4] = {"muted", "deafened", "camera", "screen"};
        for (int i = 0; i < 4; i++)
            if (json.isMember(keys[i])) {
                if (!json[keys[i]].isBool())
                    co_return fail(400, "bad_request");
                st[i] = json[keys[i]].asBool();
            }
        co_return voice_->state(sender, file, st[0], st[1], st[2], st[3]);
    }
    case VOICE_WATCH: {
        if (!json.isMember("tracks") || !json["tracks"].isObject())
            co_return fail(400, "bad_request");
        std::map<int, std::string> tracks;
        for (const auto &key : json["tracks"].getMemberNames()) {
            const Json::Value &v = json["tracks"][key];
            int n = key.size() >= 2 && key.size() <= 3 && key[0] == 'v' ? atoi(key.c_str() + 1) : 0;
            /* a member's camera (their id), or their shared screen ("<id>/screen") */
            auto whose = [](const std::string &w) { return user_id_ok(w) || (w.size() == 59 && w.substr(52) == "/screen" && user_id_ok(w.substr(0, 52))); };
            if (n < 1 || n > VOICE_TRACKS || key != "v" + std::to_string(n) || !(v.isNull() || (v.isString() && whose(v.asString()))))
                co_return fail(400, "bad_request");
            tracks[n] = v.isNull() ? "" : v.asString();
        }
        co_return voice_->watch(sender, file, tracks);
    }
    case VOICE_LEAVE:
        co_return voice_->leave(sender, file);
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
    case ADMIN_MUTE: {
        /* {until?: when it ends (ms)}; absent or null: until they are unmuted */
        int64_t until = 0;
        if (json.isMember("until") && !json["until"].isNull()) {
            if (!json["until"].isInt64() || json["until"].asInt64() < 1)
                co_return fail(400, "bad_request");
            until = json["until"].asInt64();
        }
        co_return co_await svc_.admin_mute(sender, member, until);
    }
    case ADMIN_UNMUTE:
        co_return co_await svc_.admin_unmute(sender, member);
    case ADMIN_ROLE_CREATE:
    case ADMIN_ROLE_EDIT: {
        /* {name?, color?: "#rrggbb" or null, hoist?, permissions?: [names]}; a new role needs its name */
        RoleEdit e;
        if (!opt_string("name", e.name))
            co_return fail(400, "bad_request");
        if (json.isMember("color")) {
            if (json["color"].isNull())
                e.color = "";
            else if (!opt_string("color", e.color) || e.color->empty())
                co_return fail(400, "bad_request");
            for (char &c : *e.color)
                c = (char)tolower((unsigned char)c);
        }
        if (json.isMember("hoist")) {
            if (!json["hoist"].isBool())
                co_return fail(400, "bad_request");
            e.hoist = json["hoist"].asBool();
        }
        if (json.isMember("permissions")) {
            const Json::Value &a = json["permissions"];
            if (!a.isArray() || a.size() > PERMISSIONS.size())
                co_return fail(400, "bad_request");
            int64_t bits = 0;
            for (const auto &v : a) {
                int64_t bit = 0;
                for (const auto &p : PERMISSIONS)
                    if (v.isString() && v.asString() == p.name)
                        bit = p.bit;
                if (!bit)
                    co_return fail(400, "unknown_permission");
                bits |= bit;
            }
            e.permissions = bits;
        }
        if (k == ADMIN_ROLE_CREATE)
            co_return co_await svc_.admin_role_create(sender, std::move(e));
        co_return co_await svc_.admin_role_edit(sender, channel, std::move(e));
    }
    case ADMIN_ROLE_ORDER: {
        /* {roles: [ids]}: every role, highest first */
        std::vector<int64_t> roles;
        if (!body_ids(json, "roles", (size_t)ROLES_MAX, roles))
            co_return fail(400, "bad_request");
        co_return co_await svc_.admin_role_order(sender, std::move(roles));
    }
    case ADMIN_ROLE_DELETE:
        co_return co_await svc_.admin_role_delete(sender, channel);
    case ADMIN_MEMBER_ROLES: {
        /* {roles: [ids]}: the roles they hold now */
        std::vector<int64_t> roles;
        if (!body_ids(json, "roles", (size_t)ROLES_MAX, roles))
            co_return fail(400, "bad_request");
        co_return co_await svc_.admin_member_roles(sender, member, std::move(roles));
    }
    case ADMIN_CHANNEL_ROLES: {
        /* {roles: null}: everyone sees it; {roles: [ids]}: operators, administrators, and members with one of these */
        std::optional<std::vector<int64_t>> roles;
        if (!json.isMember("roles"))
            co_return fail(400, "bad_request");
        if (!json["roles"].isNull()) {
            roles.emplace();
            if (!body_ids(json, "roles", (size_t)ROLES_MAX, *roles))
                co_return fail(400, "bad_request");
        }
        co_return co_await svc_.admin_channel_roles(sender, channel, std::move(roles));
    }
    case ADMIN_CHANNEL_ORDER: {
        /* {channels: [ids]}: every live channel, first to last */
        std::vector<int64_t> channels;
        if (!body_ids(json, "channels", 10000, channels))
            co_return fail(400, "bad_request");
        co_return co_await svc_.admin_channel_order(sender, std::move(channels));
    }
    case ADMIN_INVITES:
        co_return co_await svc_.admin_invites();
    case ADMIN_INVITE_CREATE: {
        /* {uses?: how many may join with it, expires?: when it stops working (ms)}; neither, and it lasts until revoked */
        int uses = 0;
        int64_t expires = 0;
        if (json.isMember("uses") && !json["uses"].isNull()) {
            if (!json["uses"].isInt() || json["uses"].asInt() < 1)
                co_return fail(400, "bad_request");
            uses = json["uses"].asInt();
        }
        if (json.isMember("expires") && !json["expires"].isNull()) {
            if (!json["expires"].isInt64() || json["expires"].asInt64() < 1)
                co_return fail(400, "bad_request");
            expires = json["expires"].asInt64();
        }
        co_return co_await svc_.admin_invite_create(sender, uses, expires);
    }
    case ADMIN_INVITE_REVOKE:
        co_return co_await svc_.admin_invite_revoke(sender, member);
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
    const InFlight doing(req->methodString(), req->getOriginalPath(), req->peerAddr().toIp()); /* named in a crash report */
    Answer a;
    Signing signing;
    try {
        a = co_await answer(req, signing);
    } catch (const drogon::orm::DrogonDbException &e) {
        LOG_ERROR << "database: " << e.base().what();
        a = fail(500, "database");
    } catch (const std::exception &e) {
        LOG_ERROR << req->methodString() << " " << req->getOriginalPath() << ": " << e.what();
        a = fail(500, "internal");
    }
    HttpResponsePtr r;
    if (a.status == UNKNOWN && signing.sealed) {
        /* Opened, then refused (someone who is not a member): the same "not found", but sealed under their request's key. Only
         * this Pond could have, so a member we removed can tell it is us saying so, and nobody else can say it for us
         * (spec/09). Someone who couldn't seal to our key gets the plain one below, like any unknown path. */
        std::string ct;
        r = not_found();
        if (seal_answer(signing.key, 404, "{\"error\":\"not_found\"}", ct)) {
            r->setContentTypeString(SEALED_TYPE);
            r->setBody(std::move(ct));
        }
    } else if (a.status == UNKNOWN) {
        r = not_found();
    } else {
        r = HttpResponse::newHttpResponse();
        r->setStatusCode((drogon::HttpStatusCode)a.status);
        bool sealed = signing.sealed && a.status != 204; /* a 204 has no body, so nothing to seal */
        if (sealed) {
            std::string ct;
            if (!seal_answer(signing.key, a.status, a.body, ct)) {
                LOG_ERROR << "cannot seal an answer";
                co_return not_found();
            }
            a.body = std::move(ct);
        }
        if (!signing.nonce.empty() && (!signing.only_ok || a.status == 200)) {
            std::string receipt = receipt_header(svc_.key(), signing.nonce, a.status, a.body);
            if (!receipt.empty())
                r->addHeader("X-Puddle-Receipt", receipt);
            else
                LOG_ERROR << "cannot sign a receipt";
        }
        if (sealed) {
            r->setContentTypeString(SEALED_TYPE);
            r->setBody(std::move(a.body));
        } else if (!a.body.empty()) {
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
