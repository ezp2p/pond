/* A real pondd against a real MariaDB, with requests signed by member identities (harness.h): joining with invites of each
 * kind, channels, messages, waiting for events, leaving and being removed, and that every failed check looks like an unknown
 * path. The admin's side is pondctl. */

#include "harness.h"

using namespace pond;

static std::string join_body(const Identity &who, const std::string &invite, const char *name)
{
    std::string profile = name ? std::string("{\"display_name\":\"") + name + "\"}" : "null";
    return "{\"invite\":" + jstr(invite) + ",\"chain\":" + jstr(who.chain) + ",\"profile\":" + profile + "}";
}

/* The token in the link `pondctl invite create` prints. */
static std::string invite(PondProc &p, const std::string &args)
{
    std::string out;
    p.ctl("invite create " + args + " 2>/dev/null", &out);
    size_t at = out.find("invite=");
    return at == std::string::npos ? "" : out.substr(at + 7, 26);
}

int main()
{
    TestDb db;
    if (!db.start()) {
        fprintf(stderr, "cannot start a MariaDB for the test\n");
        return 1;
    }
    PondProc pond(db);
    CHECK(pond.init("Test Pond", 300));
    std::string out;
    CHECK(pond.ctl("init Again", &out) != 0 && has(out, "already has a key")); /* never a second key */
    CHECK(pond.ctl("channel create general", &out) != 0 && has(out, "already a channel")); /* init made it */
    CHECK(pond.ctl("channel create 'Bad Name'") != 0);
    std::string two = invite(pond, "--uses 2"), forever = invite(pond, ""), soon = invite(pond, "--expires 1m");
    CHECK(two.size() == 26 && forever.size() == 26 && soon.size() == 26);
    CHECK(db.sql("UPDATE invites SET expires_ms = 1 WHERE token = '" + soon + "'") == 0); /* as if a minute had passed */
    CHECK(pond.up());
    const std::string P = pond.id;

    Identity ann, ben, cat, dan;

    /* ---- joining ---- */
    CHECK(pond.call(&ann, "POST", "/pond/v1/join", join_body(ann, "aaaaaaaaaaaaaaaaaaaaaaaaaa", "Ann"), "*").status == 404);
    CHECK(pond.call(&ann, "POST", "/pond/v1/join", join_body(ann, soon, "Ann"), "*").status == 404); /* expired */
    CHECK(pond.call(&ann, "POST", "/pond/v1/join", join_body(ann, two, "Ann"), P).status == 404); /* a first request is signed for "*" */
    CHECK(pond.call(nullptr, "POST", "/pond/v1/join", join_body(ann, two, "Ann"), "*").status == 404); /* unsigned */
    CHECK(pond.call(&ben, "POST", "/pond/v1/join", join_body(ann, two, "Ann"), "*").status == 404);   /* someone else's chain */
    Reply a = pond.call(&ann, "GET", "/pond/v1/info");
    CHECK(a.status == 404 && a.body == "{\"error\":\"not_found\"}"); /* not a member yet: just like a path that isn't there */

    a = pond.call(&ann, "POST", "/pond/v1/join", join_body(ann, two, "Ann"), "*");
    CHECK(a.status == 200);
    CHECK(has(a.body, "\"name\":\"Test Pond\"") && has(a.body, "\"name\":\"general\"") && has(a.body, ann.id) &&
          has(a.body, "\"display_name\":\"Ann\""));
    CHECK(pond.call(&ann, "POST", "/pond/v1/join", join_body(ann, two, "Ann B"), "*").status == 200); /* again: spends nothing */
    CHECK(pond.call(&ben, "POST", "/pond/v1/join", join_body(ben, two, "Ben"), "*").status == 200);   /* the second use */
    CHECK(pond.call(&cat, "POST", "/pond/v1/join", join_body(cat, two, "Cat"), "*").status == 404);   /* used up */
    CHECK(pond.call(&cat, "POST", "/pond/v1/join", join_body(cat, forever, nullptr), "*").status == 200);
    CHECK(pond.ctl("invite revoke " + forever) == 0);
    CHECK(pond.call(&dan, "POST", "/pond/v1/join", join_body(dan, forever, "Dan"), "*").status == 404); /* revoked */
    CHECK(pond.ctl("invite list", &out) == 0 && has(out, "used up") && has(out, "revoked") && has(out, "expired"));

    a = pond.call(&ann, "GET", "/pond/v1/info");
    CHECK(a.status == 200 && has(a.body, "Ann B"));
    CHECK(num_of(a.body, "protocol") == 1 && has(a.body, "\"features\":[]"));
    int64_t cursor = num_of(a.body, "cursor");
    CHECK(cursor > 0);
    a = pond.call(&ann, "GET", "/pond/v1/members");
    CHECK(a.status == 200 && has(a.body, ben.id) && has(a.body, cat.id) && !has(a.body, dan.id));
    CHECK(pond.ctl("member list", &out) == 0 && has(out, "Ann B") && has(out, "(no profile)"));

    /* ---- messages ---- */
    std::string ch = pond.channel("general");
    std::string send = "/pond/v1/channels/" + ch + "/send";
    a = pond.call(&ann, "POST", send, "{\"text\":\"  hello, pond  \",\"nonce\":\"n1\"}");
    CHECK(a.status == 200 && has(a.body, "\"text\":\"hello, pond\"") && has(a.body, ann.id));
    int64_t first = num_of(a.body, "id");
    Reply dup = pond.call(&ann, "POST", send, "{\"text\":\"hello, pond\",\"nonce\":\"n1\"}");
    CHECK(dup.status == 200 && num_of(dup.body, "id") == first); /* the same nonce posts nothing new */
    CHECK(pond.call(&ben, "POST", send, "{\"text\":\"hi ann\\nhow are you\"}").status == 200);
    CHECK(pond.call(&ben, "POST", send, "{\"text\":\"   \"}").status == 400);
    CHECK(pond.call(&ben, "POST", send, "{\"text\":\"" + std::string(4001, 'x') + "\"}").status == 400);
    CHECK(pond.call(&ben, "POST", send, "{\"text\":42}").status == 400);
    CHECK(pond.call(&ben, "POST", send, "not json").status == 400);
    CHECK(pond.call(&ben, "POST", "/pond/v1/channels/999/send", "{\"text\":\"lost\"}").status == 404);
    CHECK(pond.call(&dan, "POST", send, "{\"text\":\"let me in\"}").status == 404);

    a = pond.call(&cat, "GET", "/pond/v1/channels/" + ch + "/messages");
    CHECK(a.status == 200 && has(a.body, "hi ann\\nhow are you") && a.body.find("hi ann") < a.body.find("hello, pond")); /* newest first */
    a = pond.call(&cat, "GET", "/pond/v1/channels/" + ch + "/messages?limit=1");
    CHECK(a.status == 200 && !has(a.body, "hello, pond"));
    a = pond.call(&cat, "GET", "/pond/v1/channels/" + ch + "/messages?before=" + std::to_string(first + 1));
    CHECK(a.status == 200 && has(a.body, "hello, pond") && !has(a.body, "hi ann"));
    CHECK(pond.call(&cat, "GET", "/pond/v1/channels/" + ch + "/messages?limit=101").status == 400);
    CHECK(pond.call(&cat, "GET", "/pond/v1/channels/" + ch + "/messages?sort=up").status == 400);
    CHECK(pond.call(&cat, "GET", "/pond/v1/channels/" + ch + "/messages?limit=1&limit=2").status == 400);

    /* a burst past the limit is told to slow down */
    int slowed = 0;
    for (int i = 0; i < 30; i++)
        slowed += pond.call(&cat, "POST", send, "{\"text\":\"spam " + std::to_string(i) + "\"}").status == 429;
    CHECK(slowed > 0);

    /* ---- events ---- */
    a = pond.call(&cat, "GET", "/pond/v1/events?after=" + std::to_string(cursor));
    CHECK(a.status == 200 && has(a.body, "\"kind\":\"message\"") && has(a.body, "hello, pond") && has(a.body, "how are you"));
    int64_t next = num_of(a.body, "cursor");
    CHECK(next > cursor);
    a = pond.call(&cat, "GET", "/pond/v1/events?after=" + std::to_string(next));
    while (a.status == 200 && !has(a.body, "\"events\":[]")) { /* the spam above, a page at a time */
        next = num_of(a.body, "cursor");
        a = pond.call(&cat, "GET", "/pond/v1/events?after=" + std::to_string(next));
    }
    auto t0 = std::chrono::steady_clock::now();
    a = pond.call(&cat, "GET", "/pond/v1/events?after=" + std::to_string(next));
    auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    CHECK(a.status == 200 && has(a.body, "\"events\":[]") && waited >= 250); /* held, then empty */

    /* a request that is waiting is answered as soon as something happens, here from the admin's side */
    pond.down();
    CHECK(run("sed -i '' 's/events_wait_ms: 300/events_wait_ms: 5000/' " + pond.conf) == 0);
    CHECK(pond.up());
    Reply woke;
    std::thread waiter([&] { woke = pond.call(&cat, "GET", "/pond/v1/events?after=" + std::to_string(next)); });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    t0 = std::chrono::steady_clock::now();
    CHECK(pond.ctl("channel create random --topic chat") == 0);
    waiter.join();
    waited = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    CHECK(woke.status == 200 && has(woke.body, "\"kind\":\"channel\"") && has(woke.body, "\"name\":\"random\"") && waited < 2000);
    next = num_of(woke.body, "cursor");
    CHECK(pond.ctl("channel rename random off-topic") == 0);
    CHECK(pond.ctl("channel delete off-topic") == 0);
    CHECK(pond.ctl("channel delete off-topic") != 0);
    CHECK(until([&] {
        a = pond.call(&cat, "GET", "/pond/v1/events?after=" + std::to_string(next));
        return has(a.body, "off-topic") && has(a.body, "channel_deleted");
    }));

    /* ---- profiles, leaving, removing ---- */
    CHECK(pond.call(&ben, "POST", "/pond/v1/profile", "{\"profile\":{\"display_name\":\"Benjamin\"}}").status == 204);
    CHECK(pond.call(&ben, "POST", "/pond/v1/profile", "{\"profile\":\"not an object\"}").status == 400);
    CHECK(has(pond.call(&ann, "GET", "/pond/v1/members").body, "{\"display_name\":\"Benjamin\"}")); /* kept as sent */
    CHECK(pond.call(&ben, "POST", "/pond/v1/leave").status == 204);
    CHECK(pond.call(&ben, "GET", "/pond/v1/info").status == 404);
    CHECK(!has(pond.call(&ann, "GET", "/pond/v1/members").body, ben.id));
    CHECK(pond.ctl("member remove " + cat.id) == 0);
    CHECK(pond.ctl("member remove " + cat.id) != 0);
    CHECK(pond.call(&cat, "GET", "/pond/v1/info").status == 404);
    a = pond.call(&ann, "GET", "/pond/v1/channels/" + ch + "/messages");
    CHECK(has(a.body, "how are you")); /* what they said stays */

    /* ---- keys ---- */
    Identity before = ann;
    ann.roll_keys();
    CHECK(pond.call(&before, "GET", "/pond/v1/info").status == 200);
    CHECK(pond.call(&ann, "GET", "/pond/v1/info").status == 404); /* not until the Pond hears of the new key */
    CHECK(pond.call(&ann, "POST", "/pond/v1/keys", "{\"chain\":" + jstr(ann.chain) + "}").status == 204);
    CHECK(pond.call(&ann, "GET", "/pond/v1/info").status == 200);
    CHECK(pond.call(&before, "GET", "/pond/v1/info").status == 404); /* a retired key starts nothing */
    CHECK(pond.call(&ann, "POST", "/pond/v1/keys", "{\"chain\":" + jstr(before.chain) + "}").status == 404); /* not by its own key */
    CHECK(pond.call(&dan, "POST", "/pond/v1/keys", "{\"chain\":" + jstr(dan.chain) + "}").status == 404);   /* not a member */

    /* ---- what nobody may ask ---- */
    CHECK(pond.call(&ann, "GET", "/pond/v1/nothing").status == 404);
    CHECK(pond.call(&ann, "POST", "/pond/v1/info").status == 404);
    CHECK(pond.call(&ann, "DELETE", "/pond/v1/info").status == 404); /* not 405 */
    CHECK(pond.call(&ann, "GET", "/pond/v1/channels/01/messages").status == 404);
    CHECK(pond.call(&ann, "GET", "/").status == 404);
    CHECK(pond.call(&ann, "GET", "/pond/v1/info", "", std::string(52, 'a')).status == 404); /* signed for another Pond */
    a = pond.call(&ann, "GET", "/pond/v1/info", "", "", -10 * 60 * 1000);
    CHECK(a.status == 401 && has(a.body, "clock_skew")); /* a member may be told their clock is off */
    std::string sig = ann.signature("GET", "/pond/v1/info", "", P);
    CHECK(https(pond.port, "GET", "/pond/v1/info", "", {"X-Puddle-Signature: " + sig}).status == 200);
    a = https(pond.port, "GET", "/pond/v1/info", "", {"X-Puddle-Signature: " + sig});
    CHECK(a.status == 401 && has(a.body, "replayed"));

    /* ---- stopping answers everyone waiting ---- */
    std::thread held([&] { woke = pond.call(&ann, "GET", "/pond/v1/events?after=999999"); });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    t0 = std::chrono::steady_clock::now();
    pond.down();
    held.join();
    waited = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    CHECK(woke.status == 200 && has(woke.body, "\"events\":[]") && waited < 2000);

    return finish("pond_test");
}
