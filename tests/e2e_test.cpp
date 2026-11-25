/* Two real Puddles (puddled, a separate program: PUDDLED in CMakeLists.txt) and a real Pond, over TLS on this machine, the
 * Puddles driven through their owner API as the app drives them: joining with invites, messages arriving while a request for
 * events is held open, a new channel, profiles, scrolling back past what a Puddle keeps, a Puddle restarting, being removed,
 * and leaving. Skipped (77) without a puddled. */

#include "harness.h"

#include <sys/stat.h>


using namespace pond;

/* ---- a Puddle ---- */

struct Node {
    std::string name, dir, conf, token;
    unsigned port = 0;
    pid_t pid = 0;

    bool make(const std::string &root, const char *who, const char *display, const std::string &extra = "")
    {
        name = who;
        dir = root + "/" + who;
        conf = dir + "/puddle.yaml";
        mkdir(dir.c_str(), 0700);
        std::ofstream(dir + "/profiles.yaml") << "profiles:\n  - id: main\n    display_name: " << display << "\n  - id: work\n    display_name: "
                                              << display << " (work)\n";
        port = free_port();
        std::ofstream(conf) << "address: 127.0.0.1\nport: " << port << "\ndata_dir: " << dir << "/data\nprofiles: " << dir << "/profiles.yaml\n" << extra;
        std::string out;
        if (run(std::string(PUDDLED) + " -c " + conf + " --init < /dev/null", &out) != 0) {
            fprintf(stderr, "%s: puddled --init: %s\n", who, out.c_str());
            return false;
        }
        return up();
    }

    bool up()
    {
        pid = spawn({PUDDLED, "-c", conf}, dir + "/log");
        if (pid <= 0 || !until([&] { return port_open(port); }, 20000))
            return false;
        std::ifstream f(dir + "/data/state/owner-token");
        std::getline(f, token);
        return !token.empty();
    }

    void down() { stop(pid); }

    ~Node() { down(); }

    std::string get(const std::string &path)
    {
        return https(port, "GET", path, "", {"Authorization: Bearer " + token}).body;
    }
    int post(const std::string &path, const std::string &body, std::string *out = nullptr)
    {
        Reply r = https(port, "POST", path, body, {"Authorization: Bearer " + token});
        if (out)
            *out = r.body;
        return r.status;
    }

    /* Upload `bytes` as a file the owner will send, a MiB at a time: its id, or "" */
    std::string upload(const std::string &bytes)
    {
        std::string out;
        if (post("/api/v1/files/uploads/create", "{\"size\":" + std::to_string(bytes.size()) + "}", &out) != 201)
            return "";
        std::string u = "/api/v1/files/uploads/" + str_of(out, "upload");
        for (size_t at = 0, n = 0; at < bytes.size(); at += 1 << 20, n++)
            if (post(u + "/chunks/" + std::to_string(n), bytes.substr(at, 1 << 20)) != 200)
                return "";
        return post(u + "/finish", "", &out) == 201 ? str_of(out, "id") : "";
    }
};

/* The id of the first message in a page ({"items": [...]}): the oldest. */
static int64_t first_id(const std::string &page)
{
    Json::Value v = parse(page);
    return v["items"].isArray() && !v["items"].empty() ? v["items"][0]["id"].asInt64() : 0;
}

int main()
{
    if (access(PUDDLED, X_OK) != 0) {
        fprintf(stderr, "e2e_test: skipped: no puddled at %s (build Puddle, or set -DPUDDLED=)\n", PUDDLED);
        return 77;
    }
    TestDb db;
    if (!db.start()) {
        fprintf(stderr, "cannot start a MariaDB for the test\n");
        return 1;
    }

    /* ---- the Pond ---- */
    PondProc pond(db);
    CHECK(pond.init("Lily Pad", 20000)); /* long enough that only news, or stopping, ends a wait */
    std::string out, once, open;
    pond.ctl("invite create --uses 1 2>/dev/null", &out);
    once = out.substr(out.find("invite=") + 7, 26);
    pond.ctl("invite create 2>/dev/null", &out);
    open = out.substr(out.find("invite=") + 7, 26);
    CHECK(pond.up());
    std::string base = "pond://127.0.0.1:" + std::to_string(pond.port) + "?invite=";
    std::string k = "&k=" + base32(pond.seal_pub, 32); /* the Pond's sealing key, as invites carry it (spec/09) */

    Node ann, ben;
    CHECK(ann.make(db.dir, "ann", "Ann"));
    CHECK(ben.make(db.dir, "ben", "Ben", "files:\n  auto_download: 1 MiB\n"));

    /* ---- joining ---- */
    CHECK(ann.post("/api/v1/ponds/join", "{\"link\":\"puddle://127.0.0.1:1\"}", &out) == 400 && has(out, "bad_link"));
    CHECK(ann.post("/api/v1/ponds/join", "{\"link\":\"" + base + once + "\"}", &out) == 400 && has(out, "bad_link")); /* no key: the old kind */
    CHECK(ann.post("/api/v1/ponds/join", "{\"link\":\"" + base + "aaaaaaaaaaaaaaaaaaaaaaaaaa" + k + "\"}", &out) == 409 && has(out, "invite_refused"));
    CHECK(ann.post("/api/v1/ponds/join", "{\"link\":\"" + base + once + k + "\",\"profile\":\"nope\"}", &out) == 404);
    CHECK(ann.post("/api/v1/ponds/join", "{\"link\":\"" + base + once + k + "\",\"profile\":\"main\"}", &out) == 201);
    CHECK(has(out, "Lily Pad") && has(out, "\"name\":\"general\"") && has(out, "\"display_name\":\"Ann\""));
    std::string pid = str_of(out, "id");
    CHECK(pid == pond.id);
    CHECK(ben.post("/api/v1/ponds/join", "{\"link\":\"" + base + once + k + "\",\"profile\":\"main\"}", &out) == 409); /* used up */
    CHECK(ben.post("/api/v1/ponds/join", "{\"link\":\"" + base + open + k + "\",\"profile\":\"main\"}", &out) == 201);
    CHECK(has(ann.get("/api/v1/ponds"), "Lily Pad"));
    CHECK(has(ann.get("/api/v1/ponds"), "\"protocol\":14,\"features\":[\"files\",\"roles\",\"receipts\",\"sealed\""));

    std::string ch = pond.channel("general");
    std::string P = "/api/v1/ponds/" + pid, msgs = P + "/channels/" + ch + "/messages";
    CHECK(until([&] { return has(ann.get(P), "\"display_name\":\"Ben\""); })); /* Ben joining reaches Ann as an event */
    CHECK(until([&] { return has(ben.get("/api/v1/ponds"), "\"status\":\"connected\""); }));

    /* ---- messages, arriving while each Puddle's request for events is held open ---- */
    CHECK(ann.post(msgs + "/send", "{\"body\":\"hello, pond\"}") == 200);
    CHECK(has(ann.get(msgs), "hello, pond")); /* at once on the sender's side */
    auto t0 = std::chrono::steady_clock::now();
    CHECK(until([&] { return has(ben.get(msgs), "hello, pond"); }, 5000));
    auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    CHECK(took < 2000); /* not a poll's worth of waiting */
    CHECK(ben.post(msgs + "/send", "{\"body\":\"hi ann\"}") == 200);
    CHECK(until([&] { return has(ann.get(msgs), "hi ann"); }));
    CHECK(ann.post(msgs + "/send", "{\"body\":\"   \"}") == 400);
    CHECK(ann.post(P + "/channels/999/messages/send", "{\"body\":\"lost\"}") == 404);

    /* ---- a channel made by the admin ---- */
    CHECK(pond.ctl("channel create random --topic 'anything goes'") == 0);
    CHECK(until([&] { return has(ben.get("/api/v1/ponds"), "\"name\":\"random\""); }));
    std::string random = pond.channel("random");
    CHECK(ben.post(P + "/channels/" + random + "/messages/send", "{\"body\":\"random thought\"}") == 200);
    CHECK(until([&] { return has(ann.get(P + "/channels/" + random + "/messages"), "random thought"); }));

    /* ---- files: sent through the Pond, fetched by itself up to the automatic limit, and on asking past it ---- */
    {
        std::string small(300 * 1024, '\0'), big(5 * (1 << 19), '\0');
        for (size_t i = 0; i < big.size(); i++)
            big[i] = (char)(i * 13 + i / 4099), small[i % small.size()] = (char)(i * 5);
        std::string sid = ann.upload(small), bid = ann.upload(big);
        CHECK(sid.size() == 64 && bid.size() == 64);
        std::string files = "[{\"id\":\"" + sid + "\",\"name\":\"small.txt\",\"mime\":\"text/plain\"},{\"id\":\"" + bid +
                            "\",\"name\":\"big.bin\",\"mime\":\"application/octet-stream\"}]";
        CHECK(pond.ctl("files max-size 2M") == 0);
        CHECK(until([&] { return has(ann.get(P), "\"max_size\":2097152"); }));
        CHECK(ann.post(msgs + "/send", "{\"files\":" + files + "}", &out) == 409 && has(out, "too_large"));
        CHECK(pond.ctl("files max-size 10M") == 0);
        CHECK(until([&] { return has(ann.get(P), "\"max_size\":10485760"); }));
        CHECK(ann.post(msgs + "/send", "{\"body\":\"two files\",\"files\":" + files + "}", &out) == 200);
        CHECK(until([&] { return has(ben.get(msgs), "big.bin"); }));
        CHECK(until([&] { return has(ben.get("/api/v1/files/" + sid), "\"available\""); }));
        CHECK(ben.get("/api/v1/files/" + sid + "/content") == small);
        std::string page = ben.get(msgs);
        CHECK(has(page, "\"name\":\"big.bin\"") && has(page, "\"state\":\"waiting\"")); /* over Ben's automatic limit */
        long long id = 0; /* the message's id comes first in it */
        sscanf(page.c_str() + page.rfind("{\"id\":", page.find("two files")), "{\"id\":%lld", &id);
        std::string mid = std::to_string(id);
        CHECK(ben.post(msgs + "/" + mid + "/files/" + bid + "/download", "") == 204);
        CHECK(until([&] { return has(ben.get("/api/v1/files/" + bid), "\"available\""); }));
        CHECK(ben.get("/api/v1/files/" + bid + "/content") == big);
        CHECK(has(ann.get(msgs), "\"state\":\"available\""));
    }

    /* ---- editing and deleting messages (protocol 14, docs/design.md, "Editing and deleting messages"): every member's Puddle
     * hears of it on the event stream, keeps the message as it reads now or forgets it, and the log is for moderators ---- */
    {
        auto id_of = [](const std::string &page, const std::string &text) {
            long long id = 0;
            size_t at = page.find(text);
            if (at != std::string::npos)
                sscanf(page.c_str() + page.rfind("{\"id\":", at), "{\"id\":%lld", &id);
            return std::to_string(id);
        };
        CHECK(ann.post(msgs + "/send", "{\"body\":\"teh frogs\"}") == 200);
        CHECK(until([&] { return has(ben.get(msgs), "teh frogs"); }));
        const std::string M = id_of(ann.get(msgs), "teh frogs");
        CHECK(M != "0");
        CHECK(ben.post(msgs + "/" + M + "/edit", "{\"body\":\"mine\"}", &out) == 404); /* only its author */
        CHECK(ann.post(msgs + "/" + M + "/edit", "{\"body\":\"the frogs\"}", &out) == 200 && has(out, "\"text\":\"the frogs\"") &&
              has(out, "\"edited\":"));
        CHECK(has(ann.get(msgs), "the frogs") && !has(ann.get(msgs), "teh frogs")); /* at once on the author's side */
        CHECK(until([&] { std::string m = ben.get(msgs); return has(m, "the frogs") && !has(m, "teh frogs") && has(m, "\"edited\":"); }, 5000));
        /* deleting one's own: gone from both Puddles */
        CHECK(ann.post(msgs + "/send", "{\"body\":\"oops, wrong pond\"}") == 200);
        CHECK(until([&] { return has(ben.get(msgs), "oops, wrong pond"); }));
        const std::string W = id_of(ann.get(msgs), "oops, wrong pond");
        CHECK(ben.post(msgs + "/" + W + "/delete", "", &out) == 409 && has(out, "no_permission")); /* not his, and no say over it */
        CHECK(ann.post(msgs + "/" + W + "/delete", "") == 204);
        CHECK(!has(ann.get(msgs), "oops, wrong pond"));
        CHECK(until([&] { return !has(ben.get(msgs), "oops, wrong pond"); }, 5000));
        CHECK(ann.post(msgs + "/" + W + "/delete", "", &out) == 404);
        /* an administrator deletes anyone's, and reads the log: what it said before, and what was deleted */
        CHECK(ben.get(P + "/channels/" + ch + "/log").find("oops") == std::string::npos);
        CHECK(pond.ctl("member offer Ben administrator --yes") == 0);
        CHECK(until([&] { return has(ben.get(P), "\"offer\":{"); }));
        CHECK(ben.post(P + "/offer/accept", "", &out) == 200);
        CHECK(until([&] { return has(ben.get(P + "/channels/" + ch + "/log"), "oops, wrong pond"); }));
        std::string log = ben.get(P + "/channels/" + ch + "/log");
        CHECK(has(log, "teh frogs") && has(log, "\"deleted\":{\"ts\":") && has(log, "\"versions\":[{\"text\":\"teh frogs\""));
        CHECK(has(ben.get(msgs + "/" + M + "/versions"), "teh frogs"));
        CHECK(ben.post(msgs + "/" + M + "/delete", "") == 204);
        CHECK(!has(ben.get(msgs), "the frogs"));
        CHECK(until([&] { return !has(ann.get(msgs), "the frogs"); }, 5000));
        /* a deleted message's file, over Ben's automatic limit so never fetched: the log still gives it to him, to save */
        {
            std::string evidence(3 * (1 << 19), '\0');
            for (size_t i = 0; i < evidence.size(); i++)
                evidence[i] = (char)(i * 29 + i / 1021);
            std::string eid = ann.upload(evidence);
            CHECK(ann.post(msgs + "/send", "{\"body\":\"look\",\"files\":[{\"id\":\"" + eid + "\",\"name\":\"e.bin\",\"mime\":\"x/y\"}]}") == 200);
            CHECK(until([&] { return has(ben.get(msgs), "e.bin"); }));
            const std::string E = id_of(ann.get(msgs), "\"look\"");
            CHECK(ann.post(msgs + "/" + E + "/delete", "") == 204);
            CHECK(until([&] { return !has(ben.get(msgs), "e.bin"); }));
            CHECK(has(ben.get(P + "/channels/" + ch + "/log"), "e.bin"));
            CHECK(ben.post(msgs + "/" + E + "/files/" + eid + "/download", "") == 204);
            CHECK(until([&] { return has(ben.get("/api/v1/files/" + eid), "\"available\""); }, 15000));
            CHECK(ben.get("/api/v1/files/" + eid + "/content") == evidence);
        }
        CHECK(ben.post(msgs + "/" + M + "/purge", "") == 204);
        CHECK(!has(ben.get(P + "/channels/" + ch + "/log"), "teh frogs"));
        CHECK(pond.ctl("member role Ben member --yes") == 0);
        CHECK(until([&] { return !has(ben.get(P), "\"role\":\"administrator\""); }));
    }

    /* ---- direct messages through the Pond (docs/dms.md): sealed from one Puddle to the other, relayed, never kept ---- */
    if (!has(ann.get("/api/v1/ponds"), "\"dms\":{\"on\":true")) {
        printf("e2e_test: direct messages skipped (this puddled has none)\n");
    } else {
        std::string annId = str_of(ann.get("/api/v1/users/me"), "id"), benId = str_of(ben.get("/api/v1/users/me"), "id");
        CHECK(ann.post(P + "/dms/" + benId + "/open", "", &out) == 200 && has(out, "\"kind\":\"pond_dm\"") && has(out, "\"closed\":null"));
        std::string conv = str_of(out, "id"), C = "/api/v1/conversations/" + conv;
        CHECK(ann.post(P + "/dms/" + annId + "/open", "") == 404); /* not with ourselves */
        CHECK(ann.post(C + "/messages/send", "{\"body\":\"psst, ben\"}") == 201);
        CHECK(until([&] { return has(ben.get(C + "/messages"), "psst, ben"); })); /* the same conversation id on both sides */
        CHECK(has(ben.get("/api/v1/conversations"), "\"kind\":\"pond_dm\"") && has(ben.get(C), "\"name\":\"Lily Pad\""));
        CHECK(until([&] { return !has(ann.get(C + "/messages"), "\"delivery\""); })); /* Ben's Puddle said it came */
        {
            std::string all;
            CHECK(db.sql("SELECT data FROM events", &all) == 0 && !has(all, "psst") && !has(all, "\"dm\"")); /* nothing of it in the Pond */
        }
        CHECK(ben.post(C + "/messages/send", "{\"body\":\"hi ann, quietly\"}") == 201);
        CHECK(until([&] { return has(ann.get(C + "/messages"), "hi ann, quietly"); }));
        /* edited and deleted, as any message */
        std::string mine = ann.get(C + "/messages");
        size_t at = mine.rfind("{\"id\":\"", mine.rfind("\"conversation\"", mine.find("psst, ben")));
        std::string mid = mine.substr(at + 7, 64);
        CHECK(ann.post(C + "/messages/" + mid + "/edit", "{\"body\":\"psst, ben (edited)\"}") == 200);
        CHECK(until([&] { return has(ben.get(C + "/messages"), "psst, ben (edited)"); }));
        /* a file, a piece at a time through the Pond, which keeps none of it */
        std::string bytes(3 * (1 << 20) + 12345, '\0');
        for (size_t i = 0; i < bytes.size(); i++)
            bytes[i] = (char)(i * 7 + i / 1031);
        std::string fid = ann.upload(bytes);
        CHECK(fid.size() == 64);
        CHECK(ann.post(C + "/messages/send", "{\"body\":\"a file\",\"files\":[{\"id\":\"" + fid + "\",\"name\":\"secret.bin\",\"mime\":\"application/octet-stream\"}]}") == 201);
        CHECK(until([&] { return has(ben.get("/api/v1/files/" + fid), "\"available\""); }, 30000));
        CHECK(ben.get("/api/v1/files/" + fid + "/content") == bytes);
        /* a friend offer: Ben's side has a link to Ann's Puddle */
        CHECK(ann.post(C + "/befriend", "", &out) == 200 && has(out, "\"offered\":true"));
        CHECK(until([&] { return has(ben.get(C), "\"friend_offer\":\"puddle://"); }));
        /* away: Ann's Puddle stops; what Ben writes meanwhile reaches her when it is back (held by Ben's Puddle, or, while the
         * Pond still counts her request for events as open, in the Pond's memory) */
        ann.down();
        CHECK(ben.post(C + "/messages/send", "{\"body\":\"while you were away\"}") == 201);
        CHECK(has(ben.get(C + "/messages"), "\"delivery\""));
        CHECK(ann.up());
        CHECK(until([&] { return has(ann.get(C + "/messages"), "while you were away"); }, 90000));
        /* blocking her direct messages: dropped unread, and to her they look delivered */
        CHECK(ben.post("/api/v1/users/" + annId + "/dm-block", "", &out) == 200 && has(out, "\"dms_blocked\":true"));
        CHECK(ann.post(C + "/messages/send", "{\"body\":\"are you there?\"}") == 201);
        CHECK(until([&] { return !has(ann.get(C + "/messages"), "\"delivery\""); }));
        CHECK(!has(ben.get(C + "/messages"), "are you there?"));
        CHECK(has(ben.get("/api/v1/dm-blocks"), annId));
        CHECK(ben.post("/api/v1/users/" + annId + "/dm-unblock", "") == 200);
        /* taking none in this Pond: Ann's app is told, and her Puddle refuses to send */
        CHECK(ben.post(P + "/dms/settings", "{\"accept\":false}") == 200);
        CHECK(until([&] { return has(ann.get(C), "\"closed\":\"not_accepted\""); }));
        CHECK(ann.post(C + "/messages/send", "{\"body\":\"no?\"}", &out) == 409 && has(out, "not_accepted"));
        CHECK(ben.post(P + "/dms/settings", "{\"accept\":true}") == 200);
        CHECK(until([&] { return has(ann.get(C), "\"closed\":null"); }));
    }

    /* ---- profiles ---- */
    CHECK(ann.post(P + "/profile", "{\"profile\":\"work\"}") == 200);
    CHECK(until([&] { return has(ben.get(P), "Ann (work)"); }));
    CHECK(ann.post(P + "/profile", "{\"profile\":\"nope\"}") == 404);

    /* ---- pictures (docs/design.md, "Pictures"): Ann's profile picture reaches Ben through the Pond, and the icon an
     * administrator sets from the app reaches everyone. Skipped with a puddled from before pictures in Ponds. ---- */
    if (!has(ann.get("/api/v1/ponds"), "\"icon\":")) {
        printf("e2e_test: pictures skipped (this puddled does not show Ponds' pictures)\n");
    } else {
        std::string png = std::string("\x89PNG\r\n\x1a\n", 8) + std::string(2000, 'a'), big = std::string("\x89PNG\r\n\x1a\n", 8) + std::string(1 << 20, 'b');
        CHECK(ann.post("/api/v1/media/upload", png, &out) == 201);
        std::string hash = str_of(out, "hash");
        CHECK(hash == sha256_hex(png));
        CHECK(ann.post("/api/v1/profiles/work/edit", "{\"avatar\":\"" + hash + "\"}") == 200);
        CHECK(until([&] { return has(ben.get(P), "\"avatar\":{\"hash\":\"" + hash + "\""); })); /* Ann's Puddle sent it */
        CHECK(until([&] { return ben.get("/api/v1/media?hash=" + hash) == png; }));               /* and Ben's fetched it */

        /* the icon: a picture this Puddle holds, sent to the Pond and named; refused to someone without a role */
        CHECK(ann.post(P + "/admin/icon", "{\"hash\":\"" + hash + "\"}", &out) == 409 && has(out, "no_role"));
        CHECK(pond.ctl("member offer Ann administrator --yes") == 0);
        CHECK(until([&] { return has(ann.get(P), "\"offer\":{"); }));
        CHECK(ann.post(P + "/offer/accept", "", &out) == 200);
        CHECK(ann.post(P + "/admin/icon", "{\"hash\":\"" + std::string(64, 'c') + "\"}", &out) == 404 && has(out, "no_picture"));
        CHECK(ann.post("/api/v1/media/upload", big, &out) == 201);
        CHECK(ann.post(P + "/admin/icon", "{\"hash\":\"" + str_of(out, "hash") + "\"}", &out) == 409 && has(out, "too_large"));
        CHECK(ann.post(P + "/admin/icon", "{\"hash\":\"" + hash + "\"}", &out) == 204);
        CHECK(until([&] { return has(ben.get("/api/v1/ponds"), "\"icon\":{\"hash\":\"" + hash + "\""); }));
        CHECK(ann.post(P + "/admin/icon", "{\"hash\":null}", &out) == 204);
        CHECK(until([&] { return has(ben.get("/api/v1/ponds"), "\"icon\":null"); }));
        CHECK(pond.ctl("member role Ann member --yes") == 0);
    }

    /* ---- voice channels (docs/voice.md): two Puddles join one through their owner API, connect to the Pond, and each sees
     * the other in it; muting is heard of; leaving. Skipped with a puddled from before voice, or one built without calls. ---- */
    if (!has(ann.get("/api/v1/voice"), "\"voice\"") || has(ann.get("/api/v1/calls"), "\"version\":0,")) {
        printf("e2e_test: voice skipped (this puddled has no voice channels or no calls)\n");
    } else {
        CHECK(pond.ctl("channel create talk --voice") == 0);
        CHECK(until([&] { return has(ben.get("/api/v1/ponds"), "\"name\":\"talk\""); }));
        std::string talk = pond.channel("talk");
        CHECK(ann.post(P + "/channels/" + ch + "/voice/join", "", &out) == 409 && has(out, "not_voice"));
        CHECK(ann.post(P + "/channels/" + talk + "/voice/join", "", &out) == 200 && has(out, "\"state\":\"connecting\""));
        CHECK(ben.post(P + "/channels/" + talk + "/voice/join", "", &out) == 200);
        CHECK(until([&] { return has(ann.get("/api/v1/voice"), "\"state\":\"active\"") && has(ben.get("/api/v1/voice"), "\"state\":\"active\""); }));
        CHECK(until([&] { return count(ann.get("/api/v1/ponds"), "\"slot\":") == 2; })); /* both in it, as the Pond tells everyone */
        CHECK(ann.post("/api/v1/voice/state", "{\"muted\":true}", &out) == 200 && has(out, "\"muted\":true"));
        CHECK(until([&] { return has(ben.get("/api/v1/ponds"), "\"muted\":true"); }));
        CHECK(ann.post("/api/v1/voice/leave", "") == 204);
        CHECK(until([&] { return count(ben.get("/api/v1/ponds"), "\"slot\":") == 1; }));
        CHECK(has(ann.get("/api/v1/voice"), "\"voice\":null"));
        CHECK(ben.post("/api/v1/voice/leave", "") == 204);
        CHECK(until([&] { return count(ann.get("/api/v1/ponds"), "\"slot\":") == 0; }));
    }

    /* ---- scrolling back past what a Puddle keeps ---- */
    /* a channel with history from before anyone was watching (no events), which the members then hear of */
    CHECK(db.sql("INSERT INTO channels (name, position, created_ms) VALUES ('archive', 9, 1)") == 0);
    std::string archive = pond.channel("archive"), values;
    for (int i = 1; i <= 120; i++)
        values += std::string(i > 1 ? "," : "") + "(" + archive + ", '" + std::string(52, 'a') + "', 'old " + std::to_string(i) + "', 1)";
    CHECK(db.sql("INSERT INTO messages (channel, author, text, ts_ms) VALUES " + values) == 0);
    CHECK(pond.ctl("channel topic archive 'from before'") == 0);
    std::string A = P + "/channels/" + archive + "/messages";
    CHECK(until([&] { return has(ben.get(A), "old 120"); }));
    std::string page = ben.get(A);
    CHECK(count(page, "\"text\":") == 50 && has(page, "\"more\":true") && has(page, "old 71") && !has(page, "old 70"));
    page = ben.get(A + "?before=" + std::to_string(first_id(page))); /* asked of the Pond, and kept */
    CHECK(count(page, "\"text\":") == 50 && has(page, "old 21") && has(page, "old 70") && has(page, "\"more\":true"));
    page = ben.get(A + "?before=" + std::to_string(first_id(page)));
    CHECK(count(page, "\"text\":") == 20 && has(page, "old 1\"") && has(page, "\"more\":false"));
    page = ben.get(A + "?before=" + std::to_string(first_id(page)));
    CHECK(count(page, "\"text\":") == 0 && has(page, "\"more\":false"));
    page = ben.get(A + "?limit=100"); /* now all from what is kept */
    CHECK(count(page, "\"text\":") == 100 && has(page, "old 21\""));

    /* ---- a Puddle that restarts keeps what it held, and goes on following ---- */
    t0 = std::chrono::steady_clock::now();
    ann.down(); /* its follower was waiting on the Pond: stopping cuts that short */
    took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    CHECK(took < 5000);
    CHECK(ben.post(msgs + "/send", "{\"body\":\"while you were out\"}") == 200);
    CHECK(ann.up());
    CHECK(has(ann.get(msgs), "hello, pond"));
    CHECK(until([&] { return has(ann.get(msgs), "while you were out"); }));

    /* ---- the Pond restarting: its members wait, then carry on ---- */
    pond.down();
    CHECK(pond.up());
    CHECK(ben.post(msgs + "/send", "{\"body\":\"after the restart\"}") == 200);
    CHECK(until([&] { return has(ann.get(msgs), "after the restart"); }, 70000)); /* a follower backs off up to 60 s */

    /* ---- receipts and sealing (the Puddle Protocol, spec/05 and spec/09): someone else at the Pond's address can't pass for
     * it, nor read what is sent there ---- */
    /* another key and another certificate, as someone who had taken over the address would have; even the same database */
    pond.down();
    std::filesystem::rename(pond.dir + "/pond.key", pond.dir + "/pond.key.real");
    std::filesystem::remove(pond.dir + "/cert.pem");
    std::filesystem::remove(pond.dir + "/key.pem");
    {
        unsigned char seed[32];
        random_bytes(seed, sizeof seed);
        std::ofstream(pond.dir + "/pond.key") << hex(seed, 32) << "\n";
        chmod((pond.dir + "/pond.key").c_str(), 0600);
    }
    CHECK(pond.up());
    CHECK(ann.post(msgs + "/send", "{\"body\":\"to an impostor\"}", &out) == 502 && has(out, "not_proven"));
    CHECK(until([&] { return has(ben.get("/api/v1/ponds"), "\"status\":\"not_proven\""); }, 70000));
    /* the real Pond back, with the new certificate: renewed, as a Let's Encrypt one is every few months. Nothing depends on a
     * certificate, so its members just carry on. */
    pond.down();
    std::filesystem::rename(pond.dir + "/pond.key.real", pond.dir + "/pond.key");
    CHECK(pond.up());
    CHECK(ann.post(msgs + "/send", "{\"body\":\"after a new certificate\"}") == 200);
    CHECK(until([&] { return has(ben.get(msgs), "after a new certificate"); }, 70000));
    CHECK(until([&] { return has(ben.get("/api/v1/ponds"), "\"status\":\"connected\""); }));
    CHECK(!has(ann.get(msgs), "to an impostor"));

    /* ---- roles: offered from pondctl, accepted in the app, and used from it ---- */
    CHECK(pond.ctl("member offer " + str_of(ann.get(P), "me") + " operator --yes") == 0);
    CHECK(until([&] { return has(ann.get("/api/v1/ponds"), "\"offer\":{\"id\":"); })); /* an event for Ann alone */
    CHECK(has(ben.get("/api/v1/ponds"), "\"offer\":null"));
    CHECK(ben.post(P + "/offer/accept", "") == 404);
    CHECK(ann.post(P + "/offer/accept", "", &out) == 200 && has(out, "\"role\":\"operator\""));
    CHECK(has(ann.get("/api/v1/ponds"), "\"role\":\"operator\",\"offer\":null"));
    CHECK(until([&] { return has(ben.get(P), "\"role\":\"operator\""); }));
    CHECK(ben.post(P + "/admin/channels/create", "{\"name\":\"nope\"}", &out) == 409 && has(out, "no_role"));
    CHECK(ann.post(P + "/admin/channels/create", "{\"name\":\"news\",\"topic\":\"what is new\"}") == 204);
    CHECK(ann.post(P + "/admin/channels/create", "{\"name\":\"news\"}", &out) == 400 && has(out, "already a channel"));
    CHECK(until([&] { return has(ben.get("/api/v1/ponds"), "\"name\":\"news\""); }));
    CHECK(has(ann.get(P + "/admin/bans"), "\"bans\":[]"));

    /* ---- removed by the admin ---- */
    std::string ben_id = str_of(ben.get(P), "me");
    CHECK(ben_id.size() == 52);
    CHECK(pond.ctl("member remove " + ben_id) == 0);
    CHECK(until([&] { return has(ben.get("/api/v1/ponds"), "\"status\":\"removed\""); }));
    CHECK(until([&] { return !has(ann.get(P), ben_id); }));
    CHECK(ben.post(msgs + "/send", "{\"body\":\"am I still here\"}", &out) == 409 && has(out, "removed"));
    CHECK(ben.post(P + "/leave", "") == 204); /* forgetting it still works */
    CHECK(!has(ben.get("/api/v1/ponds"), pid));

    /* ---- leaving ---- */
    CHECK(ann.post(P + "/leave", "") == 204);
    CHECK(!has(ann.get("/api/v1/ponds"), pid));
    CHECK(ann.post(msgs + "/send", "{\"body\":\"gone\"}") == 404);
    CHECK(db.sql("SELECT COUNT(*) FROM members WHERE left_ms IS NULL", &out) == 0 && out.rfind("0", 0) == 0);

    return finish("e2e_test");
}
