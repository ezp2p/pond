#pragma once

/* What the Pond's tests share. They treat pondd and pondctl as programs, the way an admin and members' Puddles meet them:
 *
 * - TestDb: a MariaDB of the test's own, in a fresh folder under /tmp, on a free local port, thrown away at the end. Needs
 *   mariadb-install-db and mariadbd on the PATH (brew install mariadb).
 * - PondProc: a pond.yaml for that database, pondctl to set it up, and pondd running until the test ends.
 * - Identity: a member's key chain and request signatures, made here from Puddle's documented formats (docs/identity.md and
 *   docs/streams.md in the Puddle repository), not with Puddle's code.
 * - https(): one HTTPS request, the certificate not checked. */

#include "encoding.h"
#include "json.h"

#include <openssl/evp.h>
#include <openssl/ssl.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>

extern char **environ;

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

inline int finish(const char *name)
{
    if (failures)
        fprintf(stderr, "%s: %d failure(s)\n", name, failures);
    else
        printf("%s: ok\n", name);
    return failures ? 1 : 0;
}

inline bool has(const std::string &s, const std::string &part)
{
    return s.find(part) != std::string::npos;
}

inline int count(const std::string &s, const std::string &part)
{
    int n = 0;
    for (size_t at = s.find(part); at != std::string::npos; at = s.find(part, at + 1))
        n++;
    return n;
}

inline int64_t wall_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

inline unsigned free_port()
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

inline bool until(const std::function<bool()> &cond, int ms = 10000)
{
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!cond()) {
        if (std::chrono::steady_clock::now() > end)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return true;
}

/* Start `argv` with its output going to `log`. The child's pid, or -1. */
inline pid_t spawn(const std::vector<std::string> &argv, const std::string &log)
{
    std::vector<char *> a;
    for (auto &s : argv)
        a.push_back(const_cast<char *>(s.c_str()));
    a.push_back(nullptr);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, log.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
    posix_spawn_file_actions_adddup2(&fa, 1, 2);
    pid_t pid;
    int rc = posix_spawnp(&pid, a[0], &fa, nullptr, a.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    return rc == 0 ? pid : -1;
}

inline void stop(pid_t &pid)
{
    if (pid > 0) {
        kill(pid, SIGTERM);
        waitpid(pid, nullptr, 0);
        pid = 0;
    }
}

/* Run a command to the end; its exit status, with what it printed in `out`. */
inline int run(const std::string &cmd, std::string *out = nullptr)
{
    FILE *p = popen((cmd + " 2>&1").c_str(), "r");
    if (!p)
        return -1;
    std::string all;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, p)) > 0)
        all.append(buf, n);
    int st = pclose(p);
    if (out)
        *out = all;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* Something that can be connected to on 127.0.0.1:`port`. */
inline bool port_open(unsigned port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons((uint16_t)port);
    bool ok = connect(fd, (sockaddr *)&a, sizeof a) == 0;
    close(fd);
    return ok;
}

/* ---- one HTTPS request ---- */

struct Reply {
    int status = -1;
    std::string body;
};

inline Reply https(unsigned port, const std::string &method, const std::string &target, const std::string &body,
                   const std::vector<std::string> &headers = {}, int timeout_s = 30)
{
    Reply r;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons((uint16_t)port);
    if (connect(fd, (sockaddr *)&a, sizeof a) != 0) {
        close(fd);
        return r;
    }
    timeval tv = {timeout_s, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
    SSL *ssl = SSL_new(ctx);
    SSL_set_fd(ssl, fd);
    if (SSL_connect(ssl) == 1) {
        std::string req = method + " " + target + " HTTP/1.1\r\nHost: x\r\nContent-Length: " + std::to_string(body.size()) +
                          "\r\nConnection: close\r\n";
        for (auto &h : headers)
            req += h + "\r\n";
        req += "\r\n" + body;
        SSL_write(ssl, req.data(), (int)req.size());
        std::string all;
        char buf[16384];
        int n;
        while ((n = SSL_read(ssl, buf, sizeof buf)) > 0)
            all.append(buf, (size_t)n);
        if (sscanf(all.c_str(), "HTTP/1.1 %d", &r.status) != 1)
            r.status = -1;
        size_t b = all.find("\r\n\r\n");
        r.body = b == std::string::npos ? "" : all.substr(b + 4);
    }
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    close(fd);
    return r;
}

/* ---- JSON in answers ---- */

inline Json::Value parse(const std::string &s)
{
    Json::Value v;
    pond::parse_object(s, v);
    return v;
}

inline int64_t num_of(const std::string &json, const char *key)
{
    Json::Value v = parse(json);
    return v.isMember(key) && v[key].isIntegral() ? v[key].asInt64() : -1;
}

inline std::string str_of(const std::string &json, const char *key)
{
    Json::Value v = parse(json);
    return v.isMember(key) && v[key].isString() ? v[key].asString() : "";
}

/* ---- a member's identity ---- */

struct KeyPair {
    EVP_PKEY *k = nullptr;
    unsigned char pub[32] = {};

    void make()
    {
        EVP_PKEY_CTX *c = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
        EVP_PKEY_keygen_init(c);
        EVP_PKEY_keygen(c, &k);
        EVP_PKEY_CTX_free(c);
        size_t n = 32;
        EVP_PKEY_get_raw_public_key(k, pub, &n);
    }
    std::string sign(const std::string &data) const
    {
        unsigned char sig[64];
        size_t n = 64;
        EVP_MD_CTX *ctx = EVP_MD_CTX_new();
        EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, k);
        EVP_DigestSign(ctx, sig, &n, (const unsigned char *)data.data(), data.size());
        EVP_MD_CTX_free(ctx);
        return pond::base64(sig, 64);
    }
    std::string b64() const { return pond::base64(pub, 32); }
    std::string hash_hex() const
    {
        unsigned char h[32];
        pond::sha256(pub, 32, h);
        return pond::hex(h, 32);
    }
};

/* A Puddle's identity: the key chain, one record per line, and the keys that sign requests. */
struct Identity {
    KeyPair sign, enc, roll;
    std::string id, chain, head_hash;
    uint64_t seq = 0;

    Identity()
    {
        sign.make();
        enc.make();
        roll.make();
        unsigned char both[64], h[32];
        memcpy(both, sign.pub, 32);
        pond::sha256(roll.pub, 32, both + 32);
        pond::sha256(both, 64, h);
        id = pond::base32(h, 32);
        std::string rec = "{\"type\":\"identity.inception\",\"seq\":0,\"signing\":\"" + sign.b64() + "\",\"encryption\":\"" + enc.b64() +
                          "\",\"next\":\"" + roll.hash_hex() + "\",\"ts\":" + std::to_string(wall_ms()) + "}";
        chain = "{\"record\":" + rec + ",\"sig\":\"" + sign.sign(rec) + "\"}\n";
        head_hash = pond::sha256_hex(rec);
    }

    /* Roll to new keys, as `puddled --roll` does: a record revealing the roll key, signed by it, the new key and the old. */
    void roll_keys()
    {
        KeyPair ns, ne, nr;
        ns.make();
        ne.make();
        nr.make();
        std::string rec = "{\"type\":\"identity.rotate\",\"seq\":" + std::to_string(seq + 1) + ",\"prev\":\"" + head_hash + "\",\"signing\":\"" +
                          ns.b64() + "\",\"encryption\":\"" + ne.b64() + "\",\"roll\":\"" + roll.b64() + "\",\"next\":\"" + nr.hash_hex() +
                          "\",\"ts\":" + std::to_string(wall_ms()) + "}";
        chain += "{\"record\":" + rec + ",\"sig\":\"" + ns.sign(rec) + "\",\"sig_roll\":\"" + roll.sign(rec) + "\",\"sig_prev\":\"" +
                 sign.sign(rec) + "\"}\n";
        head_hash = pond::sha256_hex(rec);
        seq++;
        sign = ns, enc = ne, roll = nr;
    }

    std::string kid() const { return sign.hash_hex(); }

    /* The X-Puddle-Signature header for a request to `to` (a Pond's id, or "*"), made `skew_ms` from now. */
    std::string signature(const std::string &method, const std::string &target, const std::string &body, const std::string &to,
                          int64_t skew_ms = 0) const
    {
        unsigned char raw[16];
        pond::random_bytes(raw, sizeof raw);
        std::string nonce = pond::base64(raw, sizeof raw);
        int64_t ts = wall_ms() + skew_ms;
        std::string canon = "puddle-request-v1\n" + method + "\n" + target + "\n" + pond::sha256_hex(body) + "\n" + std::to_string(ts) +
                            "\n" + nonce + "\n" + to;
        return "v1 id=" + id + "; kid=" + kid() + "; ts=" + std::to_string(ts) + "; nonce=" + nonce + "; sig=" + sign.sign(canon);
    }
};

/* ---- the database and the Pond ---- */

struct TestDb {
    std::string dir;
    unsigned port = 0;
    pid_t pid = 0;

    bool start()
    {
        char tmpl[] = "/tmp/pondt.XXXXXX";
        if (!mkdtemp(tmpl))
            return false;
        dir = tmpl;
        if (run("mariadb-install-db --no-defaults --datadir=" + dir + "/data --auth-root-authentication-method=normal --skip-test-db > " +
                dir + "/install.log") != 0) {
            fprintf(stderr, "mariadb-install-db failed: see %s/install.log\n", dir.c_str());
            return false;
        }
        port = free_port();
        pid = spawn({"mariadbd", "--no-defaults", "--datadir=" + dir + "/data", "--socket=" + dir + "/sock", "--bind-address=127.0.0.1",
                     "--port=" + std::to_string(port), "--skip-grant-tables", "--innodb-buffer-pool-size=16M", "--pid-file=" + dir + "/pid"},
                    dir + "/server.log");
        return pid > 0 && until([&] { return sql("CREATE DATABASE IF NOT EXISTS pond") == 0; }, 20000);
    }

    /* Run SQL in the database `pond` (or none, before it exists); the rows tab-separated in `out`. */
    int sql(const std::string &query, std::string *out = nullptr)
    {
        std::string q = query;
        for (size_t at = 0; (at = q.find('\'', at)) != std::string::npos; at += 4)
            q.replace(at, 1, "'\\''");
        return run("(mariadb --no-defaults -h 127.0.0.1 -P " + std::to_string(port) + " -u root -N -B" +
                       (query.rfind("CREATE DATABASE", 0) == 0 ? "" : " pond") + " -e '" + q + "' 2>/dev/null)", /* not its warnings */
                   out);
    }

    ~TestDb()
    {
        stop(pid);
        if (!dir.empty() && !getenv("POND_KEEP_TEST_DB"))
            std::filesystem::remove_all(dir);
    }
};

struct PondProc {
    TestDb &db;
    std::string dir, conf;
    unsigned port = 0;
    pid_t pid = 0;
    std::string id; /* the Pond's */

    explicit PondProc(TestDb &d) : db(d) {}

    /* Write pond.yaml and run `pondctl init <name>`. */
    bool init(const std::string &name, int events_wait_ms)
    {
        dir = db.dir + "/pond";
        conf = db.dir + "/pond.yaml";
        port = free_port();
        std::ofstream f(conf);
        f << "listen: 127.0.0.1\nport: " << port << "\ndata_dir: " << dir << "\nevents_wait_ms: " << events_wait_ms
          << "\ndatabase:\n  host: 127.0.0.1\n  port: " << db.port << "\n  user: root\n  name: pond\n";
        f.close();
        std::string out;
        if (ctl("init '" + name + "'", &out) != 0) {
            fprintf(stderr, "pondctl init: %s\n", out.c_str());
            return false;
        }
        std::string info;
        ctl("info", &info);
        size_t at = info.find("id ");
        id = at == std::string::npos ? "" : info.substr(info.find_first_not_of(' ', at + 2), 52);
        return id.size() == 52;
    }

    int ctl(const std::string &args, std::string *out = nullptr)
    {
        return run(std::string(POND_BIN_DIR) + "/pondctl -c " + conf + " " + args, out);
    }

    /* Start pondd and wait until it answers. */
    bool up()
    {
        pid = spawn({std::string(POND_BIN_DIR) + "/pondd", "-c", conf}, db.dir + "/pondd.log");
        return pid > 0 && until([&] { return port_open(port); }, 20000);
    }

    void down() { stop(pid); }

    ~PondProc() { down(); }

    /* A request signed by `who` for `to` (the Pond's id unless given), straight to pondd. */
    Reply call(const Identity *who, const std::string &method, const std::string &target, const std::string &body = "",
               const std::string &to = "", int64_t skew_ms = 0)
    {
        std::vector<std::string> h;
        if (who)
            h.push_back("X-Puddle-Signature: " + who->signature(method, target, body, to.empty() ? id : to, skew_ms));
        return https(port, method, target, body, h);
    }

    /* The id of a live channel by name, or "". */
    std::string channel(const std::string &name)
    {
        std::string out;
        db.sql("SELECT id FROM channels WHERE name = '" + name + "' AND deleted = 0", &out);
        out.erase(out.find_last_not_of("\r\n") + 1);
        return out;
    }
};
