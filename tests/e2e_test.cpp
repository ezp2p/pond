/* Two real Puddles and a real Pond, over TLS on this machine, driven through the Puddles' owner API as the app drives them:
 * joining with invites, messages arriving while a request for events is held open, a new channel, profiles, scrolling back past
 * what a Puddle keeps, a Puddle restarting, being removed, and leaving. */

#include "testdb.h"

#include "db.h"
#include "key.h"
#include "listener.h"
#include "pond.h"
#include "routes.h"

#include "base/json.h"
#include "core/config.h"
#include "core/identity.h"
#include "service/server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <openssl/ssl.h>
#include <sys/socket.h>

#include <chrono>
#include <functional>
#include <thread>

using namespace pond;

static std::string root;

static unsigned free_port()
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(fd, (sockaddr *)&a, sizeof a);
    socklen_t l = sizeof a;
    getsockname(fd, (sockaddr *)&a, &l);
    close(fd);
    return ntohs(a.sin_port);
}

static bool until(const std::function<bool()> &cond, int ms = 10000)
{
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!cond()) {
        if (std::chrono::steady_clock::now() > end)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return true;
}

static bool has(const std::string &s, const std::string &part)
{
    return s.find(part) != std::string::npos;
}

/* ---- a Puddle ---- */

struct Node {
    std::string name;
    puddle_config cfg{};
    puddle_server *srv = nullptr;
    std::thread th;
    std::string token;

    bool make(const char *who, const char *display)
    {
        name = who;
        std::string dir = root + "/" + who;
        mkdir(dir.c_str(), 0700);
        std::string pf = dir + "/profiles.yaml";
        FILE *f = fopen(pf.c_str(), "w");
        fprintf(f, "profiles:\n  - id: main\n    display_name: %s\n  - id: work\n    display_name: %s (work)\n", display, display);
        fclose(f);
        snprintf(cfg.dir, sizeof cfg.dir, "%s", dir.c_str());
        snprintf(cfg.data_dir, sizeof cfg.data_dir, "%s/data", dir.c_str());
        snprintf(cfg.address, sizeof cfg.address, "127.0.0.1");
        cfg.port = (uint16_t)free_port();
        snprintf(cfg.profile_files[0], PUDDLE_PATH_MAX, "%s", pf.c_str());
        cfg.nprofile_files = 1;
        char err[300], phrase[PUDDLE_PHRASE_LEN + 1];
        puddle_identity *id = puddle_identity_create(cfg.data_dir, phrase, err, sizeof err);
        if (!id)
            return false;
        puddle_identity_close(id);
        puddle_identity_confirm(cfg.data_dir);
        return up();
    }

    bool up()
    {
        char err[300];
        srv = puddle_server_create(&cfg, err, sizeof err);
        if (!srv) {
            fprintf(stderr, "%s: %s\n", name.c_str(), err);
            return false;
        }
        char tok[100] = "";
        FILE *f = fopen((std::string(cfg.data_dir) + "/state/owner-token").c_str(), "r");
        if (!f || !fgets(tok, sizeof tok, f))
            return false;
        fclose(f);
        token = tok;
        token.erase(token.find_last_not_of("\r\n") + 1);
        th = std::thread([s = srv] { puddle_server_run(s); });
        return true;
    }

    void down()
    {
        if (!srv)
            return;
        puddle_server_stop(srv);
        th.join();
        puddle_server_destroy(srv);
        srv = nullptr;
    }

    ~Node() { down(); }

    /* One owner request; the body of the answer in `out`. The status, or -1. */
    int api(const char *method, const std::string &path, const std::string &body, std::string &out)
    {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = htons(cfg.port);
        if (connect(fd, (sockaddr *)&a, sizeof a) != 0) {
            close(fd);
            return -1;
        }
        timeval tv = {30, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
        SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
        SSL *ssl = SSL_new(ctx);
        SSL_set_fd(ssl, fd);
        int status = -1;
        out.clear();
        if (SSL_connect(ssl) == 1) {
            std::string req = std::string(method) + " " + path + " HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer " + token +
                              "\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
            SSL_write(ssl, req.data(), (int)req.size());
            std::string all;
            char buf[16384];
            int r;
            while ((r = SSL_read(ssl, buf, sizeof buf)) > 0)
                all.append(buf, (size_t)r);
            if (sscanf(all.c_str(), "HTTP/1.1 %d", &status) != 1)
                status = -1;
            size_t b = all.find("\r\n\r\n");
            out = b == std::string::npos ? "" : all.substr(b + 4);
        }
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        close(fd);
        return status;
    }
    std::string get(const std::string &path)
    {
        std::string out;
        api("GET", path, "", out);
        return out;
    }
    int post(const std::string &path, const std::string &body, std::string *out = nullptr)
    {
        std::string o;
        int st = api("POST", path, body, o);
        if (out)
            *out = o;
        return st;
    }
};

static std::string str_member(const std::string &json, const char *key)
{
    puddle_span v;
    char buf[200] = "";
    if (puddle_json_member(json.data(), json.size(), key, &v) == 0)
        puddle_json_string(v, buf, sizeof buf);
    return buf;
}

static int count(const std::string &s, const std::string &part)
{
    int n = 0;
    for (size_t at = s.find(part); at != std::string::npos; at = s.find(part, at + 1))
        n++;
    return n;
}

/* The id of the first message in a page ({"items": [...]}): the oldest. */
static int64_t first_id(const std::string &page)
{
    size_t at = page.find("\"id\":");
    return at == std::string::npos ? 0 : strtoll(page.c_str() + at + 5, nullptr, 10);
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

    /* ---- the Pond ---- */
    PondKey key;
    CHECK(create_key(root + "/pond", key, err));
    Pool pool(tdb.cfg, 16);
    {
        auto db = pool.get();
        CHECK(db && migrate(*db, err));
    }
    Service svc(pool, key);
    svc.wait_ms = 20000; /* long enough that only news, or stopping, ends a wait */
    Routes routes(svc);
    Listener listener;
    unsigned port = free_port();
    CHECK(listener.open("127.0.0.1", port, root + "/pond", routes.handler(), err));
    std::thread serving([&] { listener.run(); });
    std::thread watcher([&] { svc.watch(); });
    CHECK(svc.set_name("Lily Pad", nullptr, err));
    CHECK(svc.channel_create("general", "", err));
    std::string once, open;
    CHECK(svc.invite_create(1, 0, once, err));
    CHECK(svc.invite_create(0, 0, open, err));
    std::string base = "pond://127.0.0.1:" + std::to_string(port) + "?invite=";

    Node ann, ben;
    CHECK(ann.make("ann", "Ann"));
    CHECK(ben.make("ben", "Ben"));

    /* ---- joining ---- */
    std::string out;
    CHECK(ann.post("/api/v1/ponds/join", "{\"link\":\"puddle://127.0.0.1:1\"}", &out) == 400 && has(out, "bad_link"));
    CHECK(ann.post("/api/v1/ponds/join", "{\"link\":\"" + base + "aaaaaaaaaaaaaaaaaaaaaaaaaa\"}", &out) == 409 && has(out, "invite_refused"));
    CHECK(ann.post("/api/v1/ponds/join", "{\"link\":\"" + base + once + "\",\"profile\":\"nope\"}", &out) == 404);
    CHECK(ann.post("/api/v1/ponds/join", "{\"link\":\"" + base + once + "\",\"profile\":\"main\"}", &out) == 201);
    CHECK(has(out, "Lily Pad") && has(out, "\"name\":\"general\"") && has(out, "\"display_name\":\"Ann\""));
    std::string pid = str_member(out, "id");
    CHECK(pid == key.id);
    CHECK(ben.post("/api/v1/ponds/join", "{\"link\":\"" + base + once + "\",\"profile\":\"main\"}", &out) == 409); /* used up */
    CHECK(ben.post("/api/v1/ponds/join", "{\"link\":\"" + base + open + "\",\"profile\":\"main\"}", &out) == 201);
    CHECK(has(ann.get("/api/v1/ponds"), "Lily Pad"));
    CHECK(has(ann.get("/api/v1/ponds"), "\"protocol\":" + std::to_string(pond::PROTOCOL) + ",\"features\":[]"));

    std::string ch;
    {
        Result r;
        auto db = pool.get();
        db->exec("SELECT id FROM channels WHERE name = 'general'", {}, &r);
        ch = text(r.rows[0][0]);
    }
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
    CHECK(svc.channel_create("random", "anything goes", err));
    CHECK(until([&] { return has(ben.get("/api/v1/ponds"), "\"name\":\"random\""); }));
    std::string random;
    {
        Result r;
        auto db = pool.get();
        db->exec("SELECT id FROM channels WHERE name = 'random'", {}, &r);
        random = text(r.rows[0][0]);
    }
    CHECK(ben.post(P + "/channels/" + random + "/messages/send", "{\"body\":\"random thought\"}") == 200);
    CHECK(until([&] { return has(ann.get(P + "/channels/" + random + "/messages"), "random thought"); }));

    /* ---- profiles ---- */
    CHECK(ann.post(P + "/profile", "{\"profile\":\"work\"}") == 200);
    CHECK(until([&] { return has(ben.get(P), "Ann (work)"); }));
    CHECK(ann.post(P + "/profile", "{\"profile\":\"nope\"}") == 404);

    /* ---- scrolling back past what a Puddle keeps ---- */
    std::string archive;
    {
        /* a channel with history from before anyone was watching (no events), which the members then hear of */
        Result r;
        auto db = pool.get();
        db->exec("INSERT INTO channels (name, position, created_ms) VALUES ('archive', 9, ?)", {now_ms()}, &r);
        archive = std::to_string(r.insert_id);
        for (int i = 1; i <= 120; i++)
            db->exec("INSERT INTO messages (channel, author, text, ts_ms) VALUES (?, ?, ?, ?)",
                     {r.insert_id, std::string(52, 'a'), "old " + std::to_string(i), now_ms()});
    }
    CHECK(svc.channel_topic("archive", "from before", err));
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

    /* ---- removed by the admin ---- */
    char ben_id[PUDDLE_USER_ID_LEN + 1];
    puddle_identity_id(puddle_server_identity(ben.srv), ben_id);
    CHECK(svc.member_remove(ben_id, err));
    CHECK(until([&] { return has(ben.get("/api/v1/ponds"), "\"status\":\"removed\""); }));
    CHECK(until([&] { return !has(ann.get(P), ben_id); }));
    CHECK(ben.post(msgs + "/send", "{\"body\":\"am I still here\"}", &out) == 409 && has(out, "removed"));
    CHECK(ben.post(P + "/leave", "") == 204); /* forgetting it still works */
    CHECK(!has(ben.get("/api/v1/ponds"), pid));

    /* ---- leaving ---- */
    CHECK(ann.post(P + "/leave", "") == 204);
    CHECK(!has(ann.get("/api/v1/ponds"), pid));
    CHECK(ann.post(msgs + "/send", "{\"body\":\"gone\"}") == 404);
    {
        Result r;
        auto db = pool.get();
        db->exec("SELECT COUNT(*) FROM members WHERE left_ms IS NULL", {}, &r);
        CHECK(num(r.rows[0][0]) == 0);
    }

    ann.down();
    ben.down();
    listener.stop();
    serving.join();
    svc.stop();
    watcher.join();
    listener.drain();
    if (failures)
        fprintf(stderr, "%d failure(s)\n", failures);
    else
        printf("e2e_test: ok\n");
    return failures ? 1 : 0;
}
