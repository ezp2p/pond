/* A real pondd against a real MariaDB, with requests signed by member identities (harness.h): joining with invites of each
 * kind, channels, messages, waiting for events, leaving and being removed, and that every failed check looks like an unknown
 * path. The admin's side is pondctl. */

#include "harness.h"

#include <algorithm>
#include <thread>
#include "seal.h"
#include "identity.h"
#include "reqsig.h"

using namespace pond;

/* The Pond's key, as a member's Puddle learns it from a join: hex in `pond.key`, and its id the base32 of its SHA-256. */
static unsigned char pond_pub[32];

/* What a member's Puddle makes of the receipt on `r` (the Puddle Protocol, spec/02-signatures.md). */
static Receipt receipt(const Reply &r, const std::string &pond_id)
{
    return check_receipt(r.receipt, pond_id, pond_pub, r.nonce, r.status, r.raw); /* over the answer as sent: sealed */
}

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
    {
        /* the join's answer says who the Pond is, and is signed by that key; it came sealed, so only Ann could read it */
        std::string key = parse(a.body)["pond"]["key"].asString();
        unsigned char h[32];
        CHECK(unhex(key, pond_pub, 32));
        sha256(pond_pub, 32, h);
        CHECK(base32(h, 32) == P);
        CHECK(!a.receipt.empty() && receipt(a, P) == Receipt::OK);
        CHECK(a.type == SEALED_TYPE && a.raw.size() == a.body.size() + SEAL_TAG && !has(a.raw, "Test Pond"));
        unsigned char seal_pub[32];
        CHECK(unhex(parse(a.body)["pond"]["seal_key"].asString(), seal_pub, 32) && !memcmp(seal_pub, pond.seal_pub, 32));
        Reply changed = a;
        changed.raw += " ";
        CHECK(receipt(changed, P) == Receipt::BAD); /* covers the body */
        changed = a;
        changed.status = 404;
        CHECK(receipt(changed, P) == Receipt::BAD); /* and the status */
        changed = a;
        changed.nonce = "AAAAAAAAAAAAAAAAAAAAAA==";
        CHECK(receipt(changed, P) == Receipt::BAD); /* and is for this request only */
        CHECK(receipt(a, ann.id) == Receipt::WRONG_KEY);
    }
    CHECK(has(a.body, "\"name\":\"Test Pond\"") && has(a.body, "\"name\":\"general\"") && has(a.body, ann.id) &&
          has(a.body, "\"display_name\":\"Ann\""));
    CHECK(pond.call(&ann, "POST", "/pond/v1/join", join_body(ann, two, "Ann B"), "*").status == 200); /* again: spends nothing */
    CHECK(pond.call(&ben, "POST", "/pond/v1/join", join_body(ben, two, "Ben"), "*").status == 200);   /* the second use */
    CHECK(pond.call(&cat, "POST", "/pond/v1/join", join_body(cat, two, "Cat"), "*").status == 404);   /* used up */
    CHECK(pond.call(&cat, "POST", "/pond/v1/join", join_body(cat, forever, nullptr), "*").status == 200);
    CHECK(pond.ctl("invite revoke " + forever) == 0);
    CHECK(pond.call(&dan, "POST", "/pond/v1/join", join_body(dan, forever, "Dan"), "*").status == 404); /* revoked */
    CHECK(pond.ctl("invite list", &out) == 0 && has(out, "used up") && has(out, "revoked") && has(out, "expired"));

    /* many at the same moment, with one invite: each is let in or turned away, never a 500 (they meet on the invite's row,
     * and the database stops all but one: the others are tried again) */
    {
        const std::string many = invite(pond, ""), three = invite(pond, "--uses 3");
        std::vector<Identity> crowd(12);
        std::vector<int> got(crowd.size());
        std::vector<std::thread> ts;
        for (size_t i = 0; i < crowd.size(); i++)
            ts.emplace_back([&, i] {
                got[i] = pond.call(&crowd[i], "POST", "/pond/v1/join", join_body(crowd[i], i < 6 ? many : three, "Crowd"), "*").status;
            });
        for (auto &t : ts)
            t.join();
        int in = 0, turned = 0;
        for (size_t i = 0; i < crowd.size(); i++) {
            if (!(got[i] == 200 || (i >= 6 && got[i] == 404)))
                fprintf(stderr, "join %zu: %d\n", i, got[i]);
            CHECK(got[i] == 200 || (i >= 6 && got[i] == 404));
            (got[i] == 200 ? in : turned)++;
        }
        CHECK(in == 6 + 3 && turned == 3); /* all six with the open invite, three of the six with the one good three times */
        /* joins from strangers are limited (20 at once, then one each 100 ms): let the crowd's go by before the next */
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    }

    a = pond.call(&ann, "GET", "/pond/v1/info");
    CHECK(a.status == 200 && has(a.body, "Ann B"));
    CHECK(num_of(a.body, "protocol") == 9 && has(a.body, "\"features\":[\"files\",\"roles\",\"receipts\",\"sealed\""));
    CHECK(receipt(a, P) == Receipt::OK);
    Reply b;
    /* no receipt for what anyone may be told: a failed check, a join that did not work */
    b = pond.call(&dan, "GET", "/pond/v1/info");
    CHECK(b.status == 404 && b.receipt.empty());
    b = pond.call(&dan, "POST", "/pond/v1/join", join_body(dan, forever, "Dan"), "*");
    CHECK(b.status == 404 && b.receipt.empty());
    b = pond.call(&dan, "POST", "/pond/v1/join", "{\"invite\":1,\"chain\":" + jstr(dan.chain) + "}", "*");
    CHECK(b.status == 400 && b.receipt.empty());
    /* only sealed requests (spec/09): a member who sends one plain is told so; sealed to another key, nobody is anybody */
    pond.sealed = false;
    b = pond.call(&ann, "GET", "/pond/v1/info");
    CHECK(b.status == 400 && has(b.body, "not_sealed") && receipt(b, P) == Receipt::OK);
    pond.sealed = true;
    {
        unsigned char real[32];
        memcpy(real, pond.seal_pub, 32);
        pond.seal_pub[0] ^= 1;
        b = pond.call(&ann, "GET", "/pond/v1/info");
        CHECK(b.status == 404 && b.receipt.empty());
        memcpy(pond.seal_pub, real, 32);
    }
    /* a member's own 404 (a channel that isn't there) is the Pond's answer, and signed */
    b = pond.call(&ann, "GET", "/pond/v1/channels/999/messages");
    CHECK(b.status == 404 && has(b.body, "no_channel") && receipt(b, P) == Receipt::OK);
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
    CHECK(run("sed -i.bak 's/events_wait_ms: 300/events_wait_ms: 5000/' " + pond.conf) == 0);
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

    /* ---- files ---- */
    {
        /* a file of two and a half chunks, its chunk hashes and its id as a Puddle makes them */
        std::string bytes(5 * (1 << 19), '\0');
        for (size_t i = 0; i < bytes.size(); i++)
            bytes[i] = (char)(i * 7 + i / 4099);
        std::string hashes;
        for (size_t at = 0; at < bytes.size(); at += 1 << 20) {
            unsigned char h[32];
            sha256(bytes.data() + at, std::min<size_t>(1 << 20, bytes.size() - at), h);
            hashes.append((const char *)h, 32);
        }
        std::string fid = sha256_hex(hashes), F = "/pond/v1/files/" + fid;
        std::string begin = "{\"size\":" + std::to_string(bytes.size()) + ",\"chunks\":\"" +
                            base64((const unsigned char *)hashes.data(), hashes.size()) + "\"}";
        CHECK(pond.call(&dan, "POST", "/pond/v1/join", join_body(dan, invite(pond, ""), "Dan"), "*").status == 200);
        a = pond.call(&ann, "GET", "/pond/v1/info");
        CHECK(has(a.body, "\"features\":[\"files\",\"roles\",\"receipts\",\"sealed\"") && has(a.body, "\"files\":{\"max_size\":26214400,\"chunk_size\":1048576"));
        CHECK(pond.ctl("files max-size 2M") == 0);
        a = pond.call(&ann, "POST", F, begin);
        CHECK(a.status == 413 && has(a.body, "\"max\":2097152")); /* over what the admin allows */
        CHECK(pond.ctl("files max-size off") == 0);
        CHECK(has(pond.call(&ann, "POST", F, begin).body, "files_off"));
        CHECK(pond.ctl("files max-size 10M") == 0 && pond.ctl("files", &out) == 0 && has(out, "10.0 MiB"));
        CHECK(pond.call(&ann, "POST", "/pond/v1/files/" + std::string(64, 'a'), begin).status == 400); /* not this file's hashes */
        a = pond.call(&ann, "POST", F, begin);
        CHECK(a.status == 200 && has(a.body, "\"missing\":[0,1,2]"));
        a = pond.call(&ann, "POST", send, "{\"files\":[{\"id\":\"" + fid + "\",\"name\":\"a.bin\",\"mime\":\"x/y\"}]}");
        CHECK(a.status == 409 && has(a.body, "no_file")); /* not here whole yet */
        CHECK(pond.call(&ann, "POST", F + "/chunks/1", bytes.substr(0, 1 << 20)).status == 400); /* chunk 0's bytes */
        CHECK(pond.call(&ann, "POST", F + "/chunks/2", bytes.substr(2 << 20, 100)).status == 400);  /* too short */
        CHECK(pond.call(&ann, "POST", F + "/chunks/1", bytes.substr(1 << 20, 1 << 20)).status == 204);
        CHECK(has(pond.call(&ann, "POST", F, begin).body, "\"missing\":[0,2]")); /* picks up where it stopped */
        CHECK(pond.call(&dan, "GET", F + "/chunks").status == 404); /* not whole, and no message carries it */
        CHECK(pond.call(&ann, "POST", F + "/chunks/0", bytes.substr(0, 1 << 20)).status == 204);
        CHECK(pond.call(&ann, "POST", F + "/chunks/2", bytes.substr(2 << 20)).status == 204);
        CHECK(has(pond.call(&ann, "POST", F, begin).body, "\"missing\":[]"));
        CHECK(pond.call(&dan, "GET", F + "/chunks").status == 404); /* whole, but no message carries it yet */
        a = pond.call(&ann, "GET", F + "/chunks");
        CHECK(a.status == 200 && a.body == hashes); /* its sender may */
        a = pond.call(&ann, "POST", send, "{\"text\":\"a file\",\"files\":[{\"id\":\"" + fid +
                                           "\",\"name\":\"../a\\u0001.bin\",\"mime\":\"x/y\"}]}");
        CHECK(a.status == 400 && has(a.body, "bad_file")); /* no folders in a name */
        a = pond.call(&ann, "POST", send, "{\"files\":[{\"id\":\"" + fid + "\",\"name\":\"a.bin\",\"mime\":\"Application/Thing\",\"w\":4,\"h\":3,"
                                           "\"preview\":\"AAAA\"}],\"nonce\":\"f1\"}");
        CHECK(a.status == 200 && has(a.body, "\"text\":\"\"") &&
              has(a.body, "\"files\":[{\"id\":\"" + fid + "\",\"name\":\"a.bin\",\"mime\":\"application/thing\",\"size\":2621440,\"w\":4,\"h\":3,\"preview\":\"AAAA\"}]"));
        CHECK(pond.call(&ann, "POST", send, "{\"files\":[{\"id\":\"" + fid + "\",\"name\":\"a.bin\",\"mime\":\"x/y\"}],\"nonce\":\"f1\"}").body == a.body);
        a = pond.call(&dan, "GET", "/pond/v1/channels/" + ch + "/messages?limit=1");
        CHECK(has(a.body, "\"name\":\"a.bin\"") && has(a.body, "\"size\":2621440"));
        CHECK(pond.call(&dan, "GET", F + "/chunks").body == hashes);
        std::string got;
        for (int n = 0; n < 3; n++) {
            Reply c = pond.call(&dan, "GET", F + "/chunks/" + std::to_string(n));
            CHECK(receipt(c, P) == Receipt::OK); /* over the bytes of the chunk */
            got += c.body;
        }
        CHECK(got == bytes);
        CHECK(pond.call(&dan, "GET", F + "/chunks/3").status == 404);
        CHECK(pond.call(&ben, "GET", F + "/chunks/0").status == 404); /* not a member */
        CHECK(pond.call(&dan, "GET", F + "/chunks/00").status == 404);
        std::string many = "{\"text\":\"x\",\"files\":[";
        for (int i = 0; i < 11; i++)
            many += std::string(i ? "," : "") + "{\"id\":\"" + fid + "\",\"name\":\"a\",\"mime\":\"x/y\"}";
        CHECK(pond.call(&ann, "POST", send, many + "]}").status == 400);
    }

    /* ---- roles: offered from pondctl, accepted in the app; rank decides who may act on whom ---- */
    {
        Identity eve;
        CHECK(pond.call(&eve, "POST", "/pond/v1/join", join_body(eve, invite(pond, ""), "Eve"), "*").status == 200);
        CHECK(pond.call(&ann, "POST", "/pond/v1/admin/channels/create", "{\"name\":\"ops\"}").status == 404); /* no role: no such route */
        CHECK(pond.ctl("member list", &out) == 0 && has(out, "member"));
        int64_t c0 = num_of(pond.call(&ann, "GET", "/pond/v1/info").body, "cursor");
        CHECK(pond.ctl("member offer Da operator < /dev/null", &out) != 0 && has(out, "nothing changed")); /* not confirmed */
        CHECK(pond.ctl("member offer nobody-at-all operator --yes") != 0);
        CHECK(pond.ctl("member offer Dan operator --yes", &out) == 0 && has(out, dan.id));
        a = pond.call(&dan, "GET", "/pond/v1/info");
        CHECK(has(a.body, "\"role\":\"member\"") && has(a.body, "\"offer\":{\"id\":"));
        int64_t offer = parse(a.body)["offer"]["id"].asInt64();
        CHECK(has(pond.call(&ann, "GET", "/pond/v1/info").body, "\"offer\":null"));
        a = pond.call(&ann, "GET", "/pond/v1/events?after=" + std::to_string(c0));
        CHECK(a.status == 200 && !has(a.body, "role_offer") && !has(a.body, "reset") && num_of(a.body, "cursor") > c0); /* for Dan alone */
        CHECK(has(pond.call(&dan, "GET", "/pond/v1/events?after=" + std::to_string(c0)).body, "\"kind\":\"role_offer\""));
        CHECK(pond.call(&ann, "POST", "/pond/v1/roles/answer", "{\"offer\":" + std::to_string(offer) + ",\"accept\":true}").status == 404);
        a = pond.call(&dan, "POST", "/pond/v1/roles/answer", "{\"offer\":" + std::to_string(offer) + ",\"accept\":true}");
        CHECK(a.status == 200 && has(a.body, "\"role\":\"operator\""));
        CHECK(has(pond.call(&ann, "GET", "/pond/v1/members").body, "\"role\":\"operator\""));
        CHECK(pond.call(&dan, "POST", "/pond/v1/roles/answer", "{\"offer\":" + std::to_string(offer) + ",\"accept\":true}").status == 404); /* spent */

        /* an operator runs the Pond from the app */
        CHECK(pond.call(&dan, "POST", "/pond/v1/admin/channels/create", "{\"name\":\"ops\",\"topic\":\"running things\"}").status == 204);
        a = pond.call(&dan, "POST", "/pond/v1/admin/channels/create", "{\"name\":\"ops\"}");
        CHECK(a.status == 400 && has(a.body, "already a channel"));
        std::string ops = pond.channel("ops");
        CHECK(pond.call(&dan, "POST", "/pond/v1/admin/channels/" + ops + "/edit", "{\"name\":\"operations\",\"topic\":\"t\"}").status == 204);
        CHECK(has(pond.call(&ann, "GET", "/pond/v1/info").body, "\"name\":\"operations\",\"topic\":\"t\""));
        CHECK(pond.call(&dan, "POST", "/pond/v1/admin/pond", "{\"name\":\"Renamed\"}").status == 204);
        CHECK(has(pond.call(&ann, "GET", "/pond/v1/info").body, "\"name\":\"Renamed\""));
        CHECK(pond.call(&dan, "POST", "/pond/v1/admin/channels/" + ops + "/delete").status == 204);
        CHECK(pond.call(&dan, "POST", "/pond/v1/admin/channels/" + ops + "/delete").status == 404);

        /* an administrator, below an operator */
        CHECK(pond.ctl("member role Ann operator --yes", &out) != 0 && has(out, "offered, not given"));
        CHECK(pond.ctl("member offer " + ann.id + " administrator --yes") == 0);
        a = pond.call(&ann, "GET", "/pond/v1/info");
        offer = parse(a.body)["offer"]["id"].asInt64();
        CHECK(pond.call(&ann, "POST", "/pond/v1/roles/answer", "{\"offer\":" + std::to_string(offer) + ",\"accept\":true}").status == 200);
        a = pond.call(&ann, "POST", "/pond/v1/admin/members/" + dan.id + "/remove");
        CHECK(a.status == 409 && has(a.body, "outranked")); /* nobody acts on an operator from the app */
        CHECK(has(pond.call(&ann, "POST", "/pond/v1/admin/members/" + ann.id + "/remove").body, "not_yourself"));

        /* banning: out, and refused at the door until unbanned */
        CHECK(pond.call(&ann, "POST", "/pond/v1/admin/members/" + eve.id + "/ban", "{\"reason\":\"spam\"}").status == 204);
        CHECK(pond.call(&eve, "GET", "/pond/v1/info").status == 404);
        CHECK(pond.call(&eve, "POST", "/pond/v1/join", join_body(eve, invite(pond, ""), "Eve"), "*").status == 404);
        a = pond.call(&ann, "GET", "/pond/v1/admin/bans");
        CHECK(a.status == 200 && has(a.body, eve.id) && has(a.body, "\"reason\":\"spam\"") && has(a.body, "\"display_name\":\"Eve\""));
        CHECK(pond.ctl("member bans", &out) == 0 && has(out, eve.id) && has(out, "spam"));
        CHECK(pond.call(&dan, "POST", "/pond/v1/admin/bans/" + eve.id + "/unban").status == 204);
        CHECK(pond.call(&dan, "POST", "/pond/v1/admin/bans/" + eve.id + "/unban").status == 404);
        CHECK(pond.call(&eve, "POST", "/pond/v1/join", join_body(eve, invite(pond, ""), "Eve"), "*").status == 200);
        CHECK(pond.call(&ann, "POST", "/pond/v1/admin/members/" + eve.id + "/remove").status == 204); /* a kick: back with an invite */
        CHECK(pond.call(&eve, "POST", "/pond/v1/join", join_body(eve, invite(pond, ""), "Eve"), "*").status == 200);

        /* ---- pictures: the icon, and pictures in members' profiles (docs/design.md, "Pictures") ---- */
        {
            const std::string png = std::string("\x89PNG\r\n\x1a\n", 8) + std::string(300, 'p'), H = sha256_hex(png);
            CHECK(pond.call(&eve, "POST", "/pond/v1/pictures", "not a picture at all").status == 400);
            CHECK(pond.call(&eve, "POST", "/pond/v1/pictures", std::string((1 << 20) + 1, 'x')).status == 404); /* over any body */
            a = pond.call(&eve, "POST", "/pond/v1/pictures", png);
            CHECK(a.status == 200 && has(a.body, "\"hash\":\"" + H + "\"") && has(a.body, "\"mime\":\"image/png\"") &&
                  has(a.body, "\"size\":308"));
            CHECK(pond.call(&dan, "GET", "/pond/v1/pictures/" + H).status == 404); /* sent, but nobody is shown it yet */
            std::string avatar = "{\"hash\":\"" + H + "\",\"mime\":\"image/png\",\"size\":308}";
            CHECK(pond.call(&eve, "POST", "/pond/v1/profile", "{\"profile\":{\"display_name\":\"Eve\",\"avatar\":" + avatar + "}}").status == 204);
            a = pond.call(&dan, "GET", "/pond/v1/pictures/" + H);
            CHECK(a.status == 200 && a.body == png && a.type == SEALED_TYPE);
            CHECK(pond.call(&ben, "GET", "/pond/v1/pictures/" + H).status == 404);              /* not a member */
            CHECK(pond.call(&dan, "GET", "/pond/v1/pictures/" + H.substr(0, 63) + "G").status == 404);
            CHECK(pond.call(&dan, "GET", "/pond/v1/pictures/" + std::string(64, 'a')).status == 404);

            /* the icon: an administrator's, from the app, or pondctl's */
            CHECK(has(pond.call(&eve, "GET", "/pond/v1/info").body, "\"icon\":null"));
            CHECK(pond.call(&eve, "POST", "/pond/v1/admin/icon", "{\"hash\":\"" + H + "\"}").status == 404); /* no role */
            CHECK(pond.call(&ann, "POST", "/pond/v1/admin/icon", "{\"hash\":\"" + std::string(64, 'b') + "\"}").status == 400); /* not sent */
            CHECK(pond.call(&ann, "POST", "/pond/v1/admin/icon", "{}").status == 400);
            int64_t c1 = num_of(pond.call(&ann, "GET", "/pond/v1/info").body, "cursor");
            CHECK(pond.call(&ann, "POST", "/pond/v1/admin/icon", "{\"hash\":\"" + H + "\"}").status == 204);
            CHECK(has(pond.call(&eve, "GET", "/pond/v1/info").body, "\"icon\":" + avatar));
            a = pond.call(&eve, "GET", "/pond/v1/events?after=" + std::to_string(c1));
            CHECK(has(a.body, "\"kind\":\"pond\"") && has(a.body, "\"icon\":" + avatar));
            CHECK(pond.call(&ann, "POST", "/pond/v1/admin/icon", "{\"hash\":null}").status == 204);
            CHECK(has(pond.call(&eve, "GET", "/pond/v1/info").body, "\"icon\":null"));
            const std::string gif = std::string("GIF89a") + std::string(50, 'g'), file = pond.dir + "/icon.gif";
            FILE *f = fopen(file.c_str(), "wb");
            CHECK(f && fwrite(gif.data(), 1, gif.size(), f) == gif.size() && fclose(f) == 0);
            CHECK(pond.ctl("icon " + file) == 0);
            CHECK(has(pond.call(&eve, "GET", "/pond/v1/info").body, "\"icon\":{\"hash\":\"" + sha256_hex(gif) + "\",\"mime\":\"image/gif\""));
            a = pond.call(&eve, "GET", "/pond/v1/pictures/" + sha256_hex(gif));
            CHECK(a.status == 200 && a.body == gif);
            CHECK(pond.ctl("icon /no/such/file", &out) != 0);
            CHECK(pond.ctl("log", &out) == 0 && has(out, "set_icon"));
        }

        /* ---- invites from the app: made with uses and an expiry, listed while they can be used, revoked ---- */
        {
            CHECK(pond.call(&eve, "GET", "/pond/v1/admin/invites").status == 404); /* no role */
            CHECK(pond.call(&eve, "POST", "/pond/v1/admin/invites/create", "{}").status == 404);
            CHECK(pond.call(&ann, "POST", "/pond/v1/admin/invites/create", "{\"uses\":0}").status == 400);
            CHECK(pond.call(&ann, "POST", "/pond/v1/admin/invites/create", "{\"uses\":\"two\"}").status == 400);
            CHECK(pond.call(&ann, "POST", "/pond/v1/admin/invites/create", "{\"expires\":1000}").status == 400); /* long past */
            int64_t soon = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count() + 3600 * 1000;
            a = pond.call(&ann, "POST", "/pond/v1/admin/invites/create", "{\"uses\":1,\"expires\":" + std::to_string(soon) + "}");
            CHECK(a.status == 200 && has(a.body, "\"uses\":0") && has(a.body, "\"max_uses\":1") && has(a.body, "\"expires\":" + std::to_string(soon)));
            std::string token = parse(a.body)["token"].asString(), link = parse(a.body)["link"].asString();
            CHECK(token.size() == 26 && link == "pond://127.0.0.1:" + std::to_string(pond.port) + "?invite=" + token + "&k=" + base32(pond.seal_pub, 32));
            a = pond.call(&dan, "POST", "/pond/v1/admin/invites/create", "{\"uses\":null,\"expires\":null}"); /* an operator too; forever */
            CHECK(a.status == 200 && has(a.body, "\"max_uses\":null") && has(a.body, "\"expires\":null"));
            std::string forever = parse(a.body)["token"].asString();
            a = pond.call(&ann, "GET", "/pond/v1/admin/invites");
            CHECK(a.status == 200 && has(a.body, token) && has(a.body, forever) && has(a.body, "\"link\":\"pond://"));
            /* used up: it works once, then is no longer listed */
            Identity fay;
            CHECK(pond.call(&fay, "POST", "/pond/v1/join", join_body(fay, token, "Fay"), "*").status == 200);
            CHECK(!has(pond.call(&ann, "GET", "/pond/v1/admin/invites").body, token));
            /* revoked: refused at the door, gone from the list, and revoking again is 404 */
            CHECK(pond.call(&ann, "POST", "/pond/v1/admin/invites/" + forever + "/revoke").status == 204);
            CHECK(pond.call(&ann, "POST", "/pond/v1/admin/invites/" + forever + "/revoke").status == 404);
            CHECK(pond.call(&ann, "POST", "/pond/v1/admin/invites/aaaaaaaaaaaaaaaaaaaaaaaaaa/revoke").status == 404);
            CHECK(pond.call(&ann, "POST", "/pond/v1/admin/invites/short/revoke").status == 404);
            Identity gus;
            CHECK(pond.call(&gus, "POST", "/pond/v1/join", join_body(gus, forever, "Gus"), "*").status == 404);
            CHECK(!has(pond.call(&ann, "GET", "/pond/v1/admin/invites").body, forever));
            CHECK(pond.ctl("log", &out) == 0 && has(out, "invite_create") && has(out, "invite_revoke"));
            CHECK(pond.ctl("invite create", &out) == 0 && has(out, "pond://127.0.0.1:" + std::to_string(pond.port) + "?invite=")); /* pondctl's still */
            CHECK(pond.call(&fay, "POST", "/pond/v1/leave").status == 204);
        }

        /* ---- muting (docs/design.md, "Muting"): what they write is refused until it ends or they are unmuted ---- */
        {
            CHECK(pond.call(&eve, "POST", "/pond/v1/admin/members/" + ann.id + "/mute", "{}").status == 404); /* no role */
            CHECK(has(pond.call(&ann, "POST", "/pond/v1/admin/members/" + dan.id + "/mute", "{}").body, "outranked"));
            CHECK(has(pond.call(&ann, "POST", "/pond/v1/admin/members/" + ann.id + "/mute", "{}").body, "not_yourself"));
            CHECK(pond.call(&ann, "POST", "/pond/v1/admin/members/" + eve.id + "/mute", "{\"until\":1000}").status == 400); /* past */
            CHECK(pond.call(&ann, "POST", "/pond/v1/admin/members/" + eve.id + "/mute", "{\"until\":\"soon\"}").status == 400);
            CHECK(pond.call(&eve, "POST", send, "{\"text\":\"before\"}").status == 200);
            int64_t c2 = num_of(pond.call(&ann, "GET", "/pond/v1/info").body, "cursor");
            CHECK(pond.call(&ann, "POST", "/pond/v1/admin/members/" + eve.id + "/mute", "{}").status == 204);
            a = pond.call(&eve, "POST", send, "{\"text\":\"muted?\"}");
            CHECK(a.status == 409 && has(a.body, "\"error\":\"muted\"") && has(a.body, "\"until\":null"));
            CHECK(has(pond.call(&dan, "GET", "/pond/v1/members").body, "\"muted\":{\"until\":null}"));
            CHECK(has(pond.call(&dan, "GET", "/pond/v1/events?after=" + std::to_string(c2)).body, "\"muted\":{\"until\":null}"));
            CHECK(pond.call(&ann, "POST", "/pond/v1/admin/members/" + eve.id + "/unmute").status == 204);
            CHECK(pond.call(&eve, "POST", send, "{\"text\":\"after\"}").status == 200);
            CHECK(!has(pond.call(&dan, "GET", "/pond/v1/members").body, "\"muted\":{"));
            int64_t later = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count() + 600000;
            CHECK(pond.call(&dan, "POST", "/pond/v1/admin/members/" + eve.id + "/mute", "{\"until\":" + std::to_string(later) + "}").status == 204);
            CHECK(has(pond.call(&ann, "GET", "/pond/v1/members").body, "\"muted\":{\"until\":" + std::to_string(later) + "}"));
            CHECK(pond.call(&eve, "POST", send, "{\"text\":\"muted again\"}").status == 409);
            CHECK(pond.call(&dan, "POST", "/pond/v1/admin/members/" + eve.id + "/unmute").status == 204);
            CHECK(pond.ctl("log", &out) == 0 && has(out, "mute") && has(out, "unmute"));
        }

        /* ---- direct messages (docs/dms.md): relayed from memory, never kept; files a piece at a time ---- */
        {
            std::string tok = invite(pond, "--uses 3");
            Identity hal, ivy, jo;
            CHECK(pond.call(&hal, "POST", "/pond/v1/join", join_body(hal, tok, "Hal"), "*").status == 200);
            CHECK(pond.call(&ivy, "POST", "/pond/v1/join", join_body(ivy, tok, "Ivy"), "*").status == 200);
            CHECK(pond.call(&jo, "POST", "/pond/v1/join", join_body(jo, tok, "Jo"), "*").status == 200);
            a = pond.call(&hal, "GET", "/pond/v1/info");
            CHECK(has(a.body, "\"dms\",\"dm_files\"") && has(a.body, "\"dms\":{\"on\":true,\"files_max\":26214400}"));
            const int64_t hc = num_of(a.body, "cursor");
            CHECK(has(pond.call(&hal, "GET", "/pond/v1/members").body, "\"dms\":true"));
            /* a member's chain, to seal to them; a stranger's is nobody's */
            a = pond.call(&hal, "GET", "/pond/v1/members/" + ivy.id + "/chain");
            CHECK(a.status == 200 && parse(a.body)["chain"].asString() == ivy.chain);
            CHECK(pond.call(&hal, "GET", "/pond/v1/members/" + Identity().id + "/chain").status == 404);
            CHECK(has(pond.call(&ivy, "GET", "/pond/v1/dms/files/" + std::string(64, 'a') + "/0/0?from=" + jo.id).body, "away")); /* Jo's Puddle isn't following */
            const std::string L(64, 'a'), K(64, 'b'), E = std::string(43, 'A') + "=";
            auto letter = [&](const std::string &to, const std::string &id) {
                return "{\"to\":\"" + to + "\",\"id\":\"" + id + "\",\"kid\":\"" + K + "\",\"enc\":\"" + E + "\",\"sealed\":\"c2VhbGVk\"}";
            };
            CHECK(pond.call(&hal, "POST", "/pond/v1/dms/send", letter("short", L)).status == 400);
            CHECK(pond.call(&hal, "POST", "/pond/v1/dms/send", letter(Identity().id, L)).status == 404); /* not a member */
            CHECK(pond.call(&hal, "POST", "/pond/v1/dms/send", letter(hal.id, L)).status == 404);        /* themself */
            /* Ivy's Puddle isn't following: not relayed, and nothing kept. When it asks for events, Hal's is told she's back. */
            a = pond.call(&hal, "POST", "/pond/v1/dms/send", letter(ivy.id, L));
            CHECK(a.status == 200 && has(a.body, "\"relayed\":false"));
            a = pond.call(&ivy, "GET", "/pond/v1/events?after=" + std::to_string(hc));
            CHECK(a.status == 200 && !has(a.body, "\"kind\":\"dm\""));
            a = pond.call(&hal, "GET", "/pond/v1/events?after=" + std::to_string(hc));
            CHECK(has(a.body, "\"kind\":\"dm_back\"") && has(a.body, "\"member\":\"" + ivy.id + "\"") && num_of(a.body, "cursor") >= hc);
            {
                std::string n;
                CHECK(db.sql("SELECT COUNT(*) FROM events WHERE kind LIKE 'dm%'", &n) == 0 && has(n, "0")); /* none stored */
            }
            /* now she is: relayed, to her alone, from memory */
            a = pond.call(&hal, "POST", "/pond/v1/dms/send", letter(ivy.id, L));
            CHECK(a.status == 200 && has(a.body, "\"relayed\":true"));
            a = pond.call(&ivy, "GET", "/pond/v1/events?after=" + std::to_string(hc));
            CHECK(has(a.body, "\"kind\":\"dm\"") && has(a.body, "\"from\":\"" + hal.id + "\"") && has(a.body, "\"sealed\":\"c2VhbGVk\""));
            CHECK(!has(pond.call(&jo, "GET", "/pond/v1/events?after=" + std::to_string(hc)).body, "\"kind\":\"dm\""));
            CHECK(!has(pond.call(&ivy, "GET", "/pond/v1/events?after=" + std::to_string(hc)).body, "\"kind\":\"dm\"")); /* once */
            /* a request held open is woken by a letter */
            {
                Reply got;
                std::thread w([&] { got = pond.call(&ivy, "GET", "/pond/v1/events?after=999999999"); });
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                auto t1 = std::chrono::steady_clock::now();
                CHECK(pond.call(&hal, "POST", "/pond/v1/dms/send", letter(ivy.id, std::string(64, 'c'))).status == 200);
                w.join();
                CHECK(has(got.body, std::string(64, 'c')) &&
                      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t1).count() < 2000);
            }
            /* a file's piece: Ivy asks, Hal's Puddle is told and sends it, Ivy gets it */
            {
                Reply got;
                std::thread w([&] { got = pond.call(&ivy, "GET", "/pond/v1/dms/files/" + L + "/0/hashes?from=" + hal.id); });
                bool asked = false;
                CHECK(until([&] {
                    a = pond.call(&hal, "GET", "/pond/v1/events?after=999999999");
                    return asked = has(a.body, "\"kind\":\"dm_want\"") && has(a.body, "\"part\":\"hashes\"") && has(a.body, "\"to\":\"" + ivy.id + "\"");
                }));
                CHECK(pond.call(&jo, "POST", "/pond/v1/dms/files/" + L + "/0/hashes?to=" + ivy.id, "nope").status == 404); /* not who was asked */
                CHECK(pond.call(&hal, "POST", "/pond/v1/dms/files/" + L + "/0/hashes?to=" + ivy.id, "the hashes").status == 204);
                w.join();
                CHECK(asked && got.status == 200 && got.body == "the hashes");
                CHECK(pond.call(&hal, "POST", "/pond/v1/dms/files/" + L + "/0/hashes?to=" + ivy.id, "again").status == 404); /* taken */
                CHECK(pond.call(&ivy, "GET", "/pond/v1/dms/files/" + L + "/0/99999?from=" + hal.id).status == 400); /* past the largest file */
            }
            /* taking none: refused, and everyone is told */
            CHECK(pond.call(&jo, "POST", "/pond/v1/dms/settings", "{\"accept\":false}").status == 200);
            CHECK(has(pond.call(&hal, "GET", "/pond/v1/members").body, "\"dms\":false"));
            a = pond.call(&hal, "POST", "/pond/v1/dms/send", letter(jo.id, L));
            CHECK(a.status == 409 && has(a.body, "not_accepted"));
            CHECK(pond.call(&jo, "POST", "/pond/v1/dms/settings", "{\"accept\":true}").status == 200);
            /* the admin turns them off for the whole Pond, and files on their own */
            CHECK(pond.ctl("dms off") == 0);
            CHECK(!has(pond.call(&hal, "GET", "/pond/v1/info").body, "\"dms\",") && has(pond.call(&hal, "GET", "/pond/v1/info").body, "\"on\":false"));
            CHECK(has(pond.call(&hal, "POST", "/pond/v1/dms/send", letter(ivy.id, L)).body, "dms_off"));
            CHECK(pond.ctl("dms on") == 0 && pond.ctl("dms files max-size off") == 0);
            a = pond.call(&hal, "GET", "/pond/v1/info");
            CHECK(has(a.body, "\"dms\"]") || has(a.body, "\"dms\","));
            CHECK(!has(a.body, "dm_files") && has(a.body, "\"files_max\":0"));
            CHECK(has(pond.call(&ivy, "GET", "/pond/v1/dms/files/" + L + "/0/0?from=" + hal.id).body, "files_off"));
            CHECK(pond.ctl("dms", &out) == 0 && has(out, "on") && has(out, "no files"));
            CHECK(pond.ctl("dms files max-size 25M") == 0);
            /* one a second, ten at once */
            int refused = 0;
            for (int i = 0; i < 15; i++)
                refused += has(pond.call(&jo, "POST", "/pond/v1/dms/send", letter(hal.id, L)).body, "slow_down");
            CHECK(refused >= 3);
            /* someone who leaves can't be written to */
            CHECK(pond.call(&jo, "POST", "/pond/v1/leave").status == 204);
            CHECK(pond.call(&hal, "POST", "/pond/v1/dms/send", letter(jo.id, std::string(64, 'd'))).status == 404);
        }

        /* taking a role away, and the record */
        CHECK(pond.ctl("member role " + ann.id + " member --yes") == 0);
        CHECK(pond.call(&ann, "GET", "/pond/v1/admin/bans").status == 404);
        CHECK(pond.ctl("log", &out) == 0 && has(out, "offer_role") && has(out, "accept_role") && has(out, "ban") && has(out, "channel_create"));
    }

    /* ---- roles for channels, and the channels' order (docs/design.md, "Roles for channels") ---- */
    {
        /* Dan is an operator; Ann and Fay are members */
        Identity fay;
        CHECK(pond.call(&fay, "POST", "/pond/v1/join", join_body(fay, invite(pond, ""), "Fay"), "*").status == 200);
        auto channel_id = [&](Identity &who, const std::string &name) -> int64_t {
            const Json::Value info = parse(pond.call(&who, "GET", "/pond/v1/info").body); /* kept: a range over a temporary's member dangles */
            for (const auto &c : info["channels"])
                if (c["name"].asString() == name)
                    return c["id"].asInt64();
            return 0;
        };
        CHECK(pond.call(&ann, "POST", "/pond/v1/admin/roles/create", "{\"name\":\"Mods\"}").status == 404); /* no rank */
        a = pond.call(&dan, "POST", "/pond/v1/admin/roles/create", "{\"name\":\"Mods\"}");
        CHECK(a.status == 200 && has(a.body, "\"name\":\"Mods\""));
        const std::string mods = std::to_string(num_of(a.body, "id"));
        CHECK(pond.call(&dan, "POST", "/pond/v1/admin/roles/create", "{\"name\":\"Mods\"}").status == 400); /* taken */
        CHECK(pond.call(&dan, "POST", "/pond/v1/admin/roles/create", "{\"name\":\"  \"}").status == 400);
        CHECK(has(pond.call(&fay, "GET", "/pond/v1/info").body, "\"roles\":[{\"id\":" + mods + ",\"name\":\"Mods\"}]"));

        CHECK(pond.call(&dan, "POST", "/pond/v1/admin/channels/create", "{\"name\":\"staff\"}").status == 204);
        const int64_t staff = channel_id(dan, "staff");
        const std::string S = std::to_string(staff);
        CHECK(staff > 0 && channel_id(ann, "staff") == staff); /* open to everyone at first */
        int64_t c0 = num_of(pond.call(&ann, "GET", "/pond/v1/info").body, "cursor");
        CHECK(pond.call(&ann, "POST", "/pond/v1/admin/channels/" + S + "/roles", "{\"roles\":[" + mods + "]}").status == 404);
        CHECK(pond.call(&dan, "POST", "/pond/v1/admin/channels/" + S + "/roles", "{\"roles\":[999999]}").status == 404); /* no such role */
        CHECK(pond.call(&dan, "POST", "/pond/v1/admin/channels/" + S + "/roles", "{\"roles\":[" + mods + "]}").status == 204);
        CHECK(channel_id(ann, "staff") == 0 && channel_id(fay, "staff") == 0 && channel_id(dan, "staff") == staff);
        CHECK(has(pond.call(&dan, "GET", "/pond/v1/info").body, "\"roles\":[" + mods + "]}"));
        CHECK(pond.call(&ann, "GET", "/pond/v1/channels/" + S + "/messages").status == 404);
        CHECK(pond.call(&ann, "POST", "/pond/v1/channels/" + S + "/send", "{\"text\":\"let me in\"}").status == 404);
        CHECK(pond.call(&dan, "POST", "/pond/v1/channels/" + S + "/send", "{\"text\":\"for mods only\"}").status == 200);
        a = pond.call(&ann, "GET", "/pond/v1/events?after=" + std::to_string(c0));
        CHECK(has(a.body, "\"kind\":\"channels\"") && !has(a.body, "staff") && !has(a.body, "for mods only")); /* what Ann sees now */
        CHECK(has(pond.call(&dan, "GET", "/pond/v1/events?after=" + std::to_string(c0)).body, "for mods only"));

        /* given the role, Ann sees it; Fay still does not */
        CHECK(pond.call(&dan, "POST", "/pond/v1/admin/members/" + ann.id + "/roles", "{\"roles\":[999999]}").status == 404);
        CHECK(pond.call(&ann, "POST", "/pond/v1/admin/members/" + ann.id + "/roles", "{\"roles\":[" + mods + "]}").status == 404);
        int64_t c1 = num_of(pond.call(&ann, "GET", "/pond/v1/info").body, "cursor");
        CHECK(pond.call(&dan, "POST", "/pond/v1/admin/members/" + ann.id + "/roles", "{\"roles\":[" + mods + "," + mods + "]}").status == 204);
        CHECK(channel_id(ann, "staff") == staff && channel_id(fay, "staff") == 0);
        CHECK(pond.call(&ann, "GET", "/pond/v1/channels/" + S + "/messages").status == 200);
        a = pond.call(&ann, "GET", "/pond/v1/events?after=" + std::to_string(c1));
        CHECK(has(a.body, "\"kind\":\"channels\"") && has(a.body, "staff") && has(a.body, "\"roles\":[" + mods + "]")); /* hers */
        CHECK(!has(pond.call(&fay, "GET", "/pond/v1/events?after=" + std::to_string(c1)).body, "staff"));
        CHECK(has(pond.call(&fay, "GET", "/pond/v1/members").body, "\"roles\":[" + mods + "]"));

        /* renamed, then the order */
        CHECK(pond.call(&dan, "POST", "/pond/v1/admin/roles/" + mods + "/edit", "{\"name\":\"Moderators\"}").status == 204);
        CHECK(pond.call(&dan, "POST", "/pond/v1/admin/roles/999999/edit", "{\"name\":\"x\"}").status == 404);
        CHECK(has(pond.call(&fay, "GET", "/pond/v1/info").body, "\"name\":\"Moderators\""));
        std::vector<int64_t> order;
        const Json::Value info = parse(pond.call(&dan, "GET", "/pond/v1/info").body);
        for (const auto &c : info["channels"])
            order.push_back(c["id"].asInt64());
        CHECK(order.size() >= 2 && order.back() == staff);
        std::reverse(order.begin(), order.end());
        std::string list;
        for (int64_t id : order)
            list += (list.empty() ? "" : ",") + std::to_string(id);
        CHECK(pond.call(&ann, "POST", "/pond/v1/admin/channels/order", "{\"channels\":[" + list + "]}").status == 404); /* no rank */
        CHECK(pond.call(&dan, "POST", "/pond/v1/admin/channels/order", "{\"channels\":[" + S + "]}").status == 409); /* not all */
        CHECK(pond.call(&dan, "POST", "/pond/v1/admin/channels/order", "{\"channels\":[" + list + "," + S + "]}").status == 409);
        CHECK(pond.call(&dan, "POST", "/pond/v1/admin/channels/order", "{\"channels\":[" + list + "]}").status == 204);
        CHECK(parse(pond.call(&ann, "GET", "/pond/v1/info").body)["channels"][0]["id"].asInt64() == staff);
        CHECK(parse(pond.call(&fay, "GET", "/pond/v1/info").body)["channels"][0]["id"].asInt64() == order[1]); /* staff left out */

        /* deleting the role leaves the channel to operators and administrators; then open to everyone */
        CHECK(pond.call(&dan, "POST", "/pond/v1/admin/roles/" + mods + "/delete").status == 204);
        CHECK(pond.call(&dan, "POST", "/pond/v1/admin/roles/" + mods + "/delete").status == 404);
        CHECK(channel_id(ann, "staff") == 0 && has(pond.call(&dan, "GET", "/pond/v1/info").body, "\"roles\":[]}"));
        CHECK(has(pond.call(&ann, "GET", "/pond/v1/members").body, "\"roles\":[]"));
        CHECK(pond.call(&dan, "POST", "/pond/v1/admin/channels/" + S + "/roles", "{\"roles\":null}").status == 204);
        CHECK(channel_id(fay, "staff") == staff);
        CHECK(pond.ctl("log", &out) == 0 && has(out, "role_create") && has(out, "channel_roles") && has(out, "channel_order"));
    }

    /* ---- what nobody may ask ---- */
    CHECK(pond.call(&ann, "GET", "/pond/v1/nothing").status == 404);
    CHECK(pond.call(&ann, "POST", "/pond/v1/info").status == 404);
    CHECK(pond.call(&ann, "DELETE", "/pond/v1/info").status == 404); /* not 405 */
    CHECK(pond.call(&ann, "GET", "/pond/v1/channels/01/messages").status == 404);
    CHECK(pond.call(&ann, "GET", "/").status == 404);
    CHECK(pond.call(&ann, "GET", "/pond/v1/info", "", std::string(52, 'a')).status == 404); /* signed for another Pond */
    a = pond.call(&ann, "GET", "/pond/v1/info", "", "", -10 * 60 * 1000);
    CHECK(a.status == 401 && has(a.body, "clock_skew") && receipt(a, P) == Receipt::OK); /* a member may be told their clock is off */
    {   /* the very same sealed request twice: the second is a replay */
        unsigned char raw[16], enc[32], key[32];
        random_bytes(raw, sizeof raw);
        std::string nonce = base64(raw, sizeof raw), opened;
        Hpke ctx;
        CHECK(hpke_sender(pond.seal_pub, seal_info(ann.id, P, nonce), enc, ctx) && answer_key(ctx, key));
        std::vector<std::string> h = {"X-Puddle-Signature: " + ann.signature("GET", "/pond/v1/info", "", P, 0, nonce),
                                      "X-Puddle-Seal: v1 kid=" + sha256_hex(std::string_view((const char *)pond.seal_pub, 32)) + "; enc=" + base64(enc, 32)};
        CHECK(https(pond.port, "GET", "/pond/v1/info", "", h).status == 200);
        a = https(pond.port, "GET", "/pond/v1/info", "", h);
        CHECK(a.status == 401 && open_answer(key, 401, a.raw, opened) && has(opened, "replayed"));
    }

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
