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

    bool make(const std::string &root, const char *who, const char *display)
    {
        name = who;
        dir = root + "/" + who;
        conf = dir + "/puddle.yaml";
        mkdir(dir.c_str(), 0700);
        std::ofstream(dir + "/profiles.yaml") << "profiles:\n  - id: main\n    display_name: " << display << "\n  - id: work\n    display_name: "
                                              << display << " (work)\n";
        port = free_port();
        std::ofstream(conf) << "address: 127.0.0.1\nport: " << port << "\ndata_dir: " << dir << "/data\nprofiles: " << dir << "/profiles.yaml\n";
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

    Node ann, ben;
    CHECK(ann.make(db.dir, "ann", "Ann"));
    CHECK(ben.make(db.dir, "ben", "Ben"));

    /* ---- joining ---- */
    CHECK(ann.post("/api/v1/ponds/join", "{\"link\":\"puddle://127.0.0.1:1\"}", &out) == 400 && has(out, "bad_link"));
    CHECK(ann.post("/api/v1/ponds/join", "{\"link\":\"" + base + "aaaaaaaaaaaaaaaaaaaaaaaaaa\"}", &out) == 409 && has(out, "invite_refused"));
    CHECK(ann.post("/api/v1/ponds/join", "{\"link\":\"" + base + once + "\",\"profile\":\"nope\"}", &out) == 404);
    CHECK(ann.post("/api/v1/ponds/join", "{\"link\":\"" + base + once + "\",\"profile\":\"main\"}", &out) == 201);
    CHECK(has(out, "Lily Pad") && has(out, "\"name\":\"general\"") && has(out, "\"display_name\":\"Ann\""));
    std::string pid = str_of(out, "id");
    CHECK(pid == pond.id);
    CHECK(ben.post("/api/v1/ponds/join", "{\"link\":\"" + base + once + "\",\"profile\":\"main\"}", &out) == 409); /* used up */
    CHECK(ben.post("/api/v1/ponds/join", "{\"link\":\"" + base + open + "\",\"profile\":\"main\"}", &out) == 201);
    CHECK(has(ann.get("/api/v1/ponds"), "Lily Pad"));
    CHECK(has(ann.get("/api/v1/ponds"), "\"protocol\":1,\"features\":[]"));

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

    /* ---- profiles ---- */
    CHECK(ann.post(P + "/profile", "{\"profile\":\"work\"}") == 200);
    CHECK(until([&] { return has(ben.get(P), "Ann (work)"); }));
    CHECK(ann.post(P + "/profile", "{\"profile\":\"nope\"}") == 404);

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
