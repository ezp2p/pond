/* The Pond's routes against a real MariaDB, with requests signed by real Puddle identities: joining with invites of each kind,
 * channels, messages, waiting for events, leaving and being removed, and that every failed check looks like an unknown path. */

#include "testdb.h"

#include "db.h"
#include "key.h"
#include "pond.h"
#include "routes.h"

#include "base/json.h"
#include "core/identity.h"
#include "http/reqsig.h"

#include <chrono>
#include <thread>

using namespace pond;

static std::string root;

static puddle_identity *make_identity(const char *name)
{
    char phrase[PUDDLE_PHRASE_LEN + 1], err[300];
    std::string dir = root + "/" + name;
    puddle_identity *id = puddle_identity_create(dir.c_str(), phrase, err, sizeof err);
    if (!id) {
        fprintf(stderr, "cannot make %s: %s\n", name, err);
        exit(2);
    }
    puddle_identity_confirm(dir.c_str());
    return id;
}

static std::string id_of(puddle_identity *p)
{
    char id[PUDDLE_USER_ID_LEN + 1];
    puddle_identity_id(p, id);
    return id;
}

static std::string chain_of(puddle_identity *p)
{
    size_t len;
    char *c = puddle_identity_chain(p, &len);
    std::string s(c, len);
    free(c);
    return s;
}

/* A request from `who`, signed for `to` (the Pond's id, or "*"), straight to the routes. */
static Answer call(Routes &r, puddle_identity *who, const char *method, const std::string &target, const std::string &body,
                   const std::string &to)
{
    puddle_request req = {};
    snprintf(req.method, sizeof req.method, "%s", method);
    snprintf(req.target, sizeof req.target, "%s", target.c_str());
    snprintf(req.path, sizeof req.path, "%.*s", (int)target.find('?'), target.c_str());
    req.content_length = body.size();
    if (who)
        puddle_reqsig_sign(who, method, target.c_str(), body.data(), body.size(), to.c_str(), 0, req.signature, sizeof req.signature);
    return r.handle(req, (const unsigned char *)body.c_str(), body.size());
}

static std::string join_body(puddle_identity *who, const std::string &invite, const char *name)
{
    std::string profile = name ? std::string("{\"display_name\":\"") + name + "\"}" : "null";
    return "{\"invite\":" + jstr(invite) + ",\"chain\":" + jstr(chain_of(who)) + ",\"profile\":" + profile + "}";
}

static int64_t member_num(const std::string &json, const char *key)
{
    puddle_span v;
    int64_t n = -1;
    if (puddle_json_member(json.data(), json.size(), key, &v) == 0)
        puddle_json_int64(v, &n);
    return n;
}

static bool has(const std::string &s, const std::string &part)
{
    return s.find(part) != std::string::npos;
}

int main()
{
    TestDb tdb;
    if (!tdb.start()) {
        fprintf(stderr, "cannot start a MariaDB for the test\n");
        return 1;
    }
    root = tdb.dir;
    std::string err;
    PondKey key;
    CHECK(create_key(root + "/pond", key, err));
    CHECK(!create_key(root + "/pond", key, err)); /* never a second key */
    PondKey again;
    CHECK(load_key(root + "/pond", again, err) && again.id == key.id && key.id.size() == 52);

    Pool pool(tdb.cfg, 8);
    {
        auto db = pool.get();
        CHECK(db && migrate(*db, err));
        CHECK(migrate(*db, err)); /* twice is harmless */
    }
    Service svc(pool, key);
    svc.wait_ms = 300;
    std::thread watcher([&] { svc.watch(); });
    Routes routes(svc);
    const std::string P = key.id;

    CHECK(svc.set_name("Test Pond", nullptr, err));
    CHECK(svc.channel_create("general", "", err));
    CHECK(!svc.channel_create("general", "", err)); /* names are unique */
    CHECK(!svc.channel_create("Bad Name", "", err));
    std::string two, forever, expired;
    CHECK(svc.invite_create(2, 0, two, err));
    CHECK(svc.invite_create(0, 0, forever, err));
    CHECK(svc.invite_create(0, now_ms() - 1000, expired, err));

    puddle_identity *ann = make_identity("ann"), *ben = make_identity("ben"), *cat = make_identity("cat"), *dan = make_identity("dan");

    /* ---- joining ---- */
    CHECK(call(routes, ann, "POST", "/pond/v1/join", join_body(ann, "aaaaaaaaaaaaaaaaaaaaaaaaaa", "Ann"), "*").status == -1);
    CHECK(call(routes, ann, "POST", "/pond/v1/join", join_body(ann, expired, "Ann"), "*").status == -1);
    CHECK(call(routes, ann, "POST", "/pond/v1/join", join_body(ann, two, "Ann"), P).status == -1); /* a first request is signed for "*" */
    CHECK(call(routes, nullptr, "POST", "/pond/v1/join", join_body(ann, two, "Ann"), "*").status == -1); /* unsigned */
    CHECK(call(routes, ben, "POST", "/pond/v1/join", join_body(ann, two, "Ann"), "*").status == -1);   /* someone else's chain */
    CHECK(call(routes, ann, "GET", "/pond/v1/info", "", P).status == -1);                               /* not a member yet */

    Answer a = call(routes, ann, "POST", "/pond/v1/join", join_body(ann, two, "Ann"), "*");
    CHECK(a.status == 200);
    CHECK(has(a.body, "\"name\":\"Test Pond\"") && has(a.body, "\"name\":\"general\"") && has(a.body, id_of(ann)) &&
          has(a.body, "\"display_name\":\"Ann\""));
    CHECK(call(routes, ann, "POST", "/pond/v1/join", join_body(ann, two, "Ann B"), "*").status == 200); /* again: spends nothing */
    CHECK(call(routes, ben, "POST", "/pond/v1/join", join_body(ben, two, "Ben"), "*").status == 200);   /* the second use */
    CHECK(call(routes, cat, "POST", "/pond/v1/join", join_body(cat, two, "Cat"), "*").status == -1);    /* used up */
    CHECK(call(routes, cat, "POST", "/pond/v1/join", join_body(cat, forever, nullptr), "*").status == 200);
    CHECK(svc.invite_revoke(forever, err));
    CHECK(call(routes, dan, "POST", "/pond/v1/join", join_body(dan, forever, "Dan"), "*").status == -1); /* revoked */

    a = call(routes, ann, "GET", "/pond/v1/info", "", P);
    CHECK(a.status == 200 && has(a.body, "Ann B"));
    CHECK(member_num(a.body, "protocol") == pond::PROTOCOL && has(a.body, "\"features\":[]"));
    int64_t cursor = member_num(a.body, "cursor");
    CHECK(cursor > 0);
    a = call(routes, ann, "GET", "/pond/v1/members", "", P);
    CHECK(a.status == 200 && has(a.body, id_of(ben)) && has(a.body, id_of(cat)) && !has(a.body, id_of(dan)));

    /* ---- messages ---- */
    std::string ch = "1";
    {
        Result r;
        auto db = pool.get();
        db->exec("SELECT id FROM channels WHERE name = 'general'", {}, &r);
        ch = text(r.rows[0][0]);
    }
    std::string send = "/pond/v1/channels/" + ch + "/send";
    a = call(routes, ann, "POST", send, "{\"text\":\"  hello, pond  \",\"nonce\":\"n1\"}", P);
    CHECK(a.status == 200 && has(a.body, "\"text\":\"hello, pond\"") && has(a.body, id_of(ann)));
    int64_t first = member_num(a.body, "id");
    Answer dup = call(routes, ann, "POST", send, "{\"text\":\"hello, pond\",\"nonce\":\"n1\"}", P);
    CHECK(dup.status == 200 && member_num(dup.body, "id") == first); /* the same nonce posts nothing new */
    CHECK(call(routes, ben, "POST", send, "{\"text\":\"hi ann\\nhow are you\"}", P).status == 200);
    CHECK(call(routes, ben, "POST", send, "{\"text\":\"   \"}", P).status == 400);
    CHECK(call(routes, ben, "POST", send, "{\"text\":\"" + std::string(4001, 'x') + "\"}", P).status == 400);
    CHECK(call(routes, ben, "POST", "/pond/v1/channels/999/send", "{\"text\":\"lost\"}", P).status == 404);
    CHECK(call(routes, dan, "POST", send, "{\"text\":\"let me in\"}", P).status == -1);

    a = call(routes, cat, "GET", "/pond/v1/channels/" + ch + "/messages", "", P);
    CHECK(a.status == 200 && has(a.body, "hi ann\\nhow are you") && a.body.find("hi ann") < a.body.find("hello, pond")); /* newest first */
    a = call(routes, cat, "GET", "/pond/v1/channels/" + ch + "/messages?limit=1", "", P);
    CHECK(a.status == 200 && !has(a.body, "hello, pond"));
    a = call(routes, cat, "GET", "/pond/v1/channels/" + ch + "/messages?before=" + std::to_string(first + 1), "", P);
    CHECK(a.status == 200 && has(a.body, "hello, pond") && !has(a.body, "hi ann"));
    CHECK(call(routes, cat, "GET", "/pond/v1/channels/" + ch + "/messages?limit=101", "", P).status == 400);
    CHECK(call(routes, cat, "GET", "/pond/v1/channels/" + ch + "/messages?sort=up", "", P).status == 400);

    /* ---- events ---- */
    a = call(routes, cat, "GET", "/pond/v1/events?after=" + std::to_string(cursor), "", P);
    CHECK(a.status == 200 && has(a.body, "\"kind\":\"message\"") && has(a.body, "hello, pond") && has(a.body, "how are you"));
    int64_t next = member_num(a.body, "cursor");
    CHECK(next > cursor);
    auto t0 = std::chrono::steady_clock::now();
    a = call(routes, cat, "GET", "/pond/v1/events?after=" + std::to_string(next), "", P);
    auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    CHECK(a.status == 200 && has(a.body, "\"events\":[]") && waited >= 250); /* held, then empty */

    /* a request that is waiting is answered as soon as something happens, here from the admin's side */
    Answer woke;
    std::thread waiter([&] {
        svc.wait_ms = 5000;
        woke = call(routes, cat, "GET", "/pond/v1/events?after=" + std::to_string(next), "", P);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    t0 = std::chrono::steady_clock::now();
    CHECK(svc.channel_create("random", "chat", err));
    waiter.join();
    waited = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    CHECK(woke.status == 200 && has(woke.body, "\"kind\":\"channel\"") && has(woke.body, "\"name\":\"random\"") && waited < 2000);
    svc.wait_ms = 300;
    next = member_num(woke.body, "cursor");
    CHECK(svc.channel_rename("random", "off-topic", err));
    CHECK(svc.channel_delete("off-topic", err));
    a = call(routes, cat, "GET", "/pond/v1/events?after=" + std::to_string(next), "", P);
    CHECK(has(a.body, "off-topic") && has(a.body, "channel_deleted"));

    /* ---- profiles, leaving, removing ---- */
    CHECK(call(routes, ben, "POST", "/pond/v1/profile", "{\"profile\":{\"display_name\":\"Benjamin\"}}", P).status == 204);
    CHECK(call(routes, ben, "POST", "/pond/v1/profile", "{\"profile\":\"not an object\"}", P).status == 400);
    CHECK(has(call(routes, ann, "GET", "/pond/v1/members", "", P).body, "Benjamin"));
    CHECK(call(routes, ben, "POST", "/pond/v1/leave", "", P).status == 204);
    CHECK(call(routes, ben, "GET", "/pond/v1/info", "", P).status == -1);
    CHECK(!has(call(routes, ann, "GET", "/pond/v1/members", "", P).body, id_of(ben)));
    CHECK(svc.member_remove(id_of(cat), err));
    CHECK(!svc.member_remove(id_of(cat), err));
    CHECK(call(routes, cat, "GET", "/pond/v1/info", "", P).status == -1);
    a = call(routes, ann, "GET", "/pond/v1/channels/" + ch + "/messages", "", P);
    CHECK(has(a.body, "how are you")); /* what they said stays */

    /* ---- keys ---- */
    CHECK(call(routes, ann, "POST", "/pond/v1/keys", "{\"chain\":" + jstr(chain_of(ann)) + "}", P).status == 204);
    CHECK(call(routes, dan, "POST", "/pond/v1/keys", "{\"chain\":" + jstr(chain_of(dan)) + "}", P).status == -1);

    /* ---- what nobody may ask ---- */
    CHECK(call(routes, ann, "GET", "/pond/v1/nothing", "", P).status == -1);
    CHECK(call(routes, ann, "POST", "/pond/v1/info", "", P).status == -1);
    CHECK(call(routes, ann, "GET", "/pond/v1/channels/01/messages", "", P).status == -1);
    CHECK(call(routes, ann, "GET", "/pond/v1/info", "", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa").status == -1); /* another Pond */

    svc.stop();
    watcher.join();
    for (auto *p : {ann, ben, cat, dan})
        puddle_identity_close(p);
    if (failures)
        fprintf(stderr, "%d failure(s)\n", failures);
    else
        printf("pond_test: ok\n");
    return failures ? 1 : 0;
}
