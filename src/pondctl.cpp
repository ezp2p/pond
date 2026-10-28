/* pondctl: the Pond admin's tool (docs/design.md, "pondctl"). It works on the database and data folder pondd uses, so it runs
 * beside it on the same machine; pondd notices its changes within half a second. */

#include "config.h"
#include "db.h"
#include "key.h"
#include "pond.h"

#include "http/tls.h"

#include <openssl/ssl.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

using namespace pond;

static const char USAGE[] =
    "usage: pondctl [-c pond.yaml] <command>\n"
    "\n"
    "  init <name>                          make the key and the database tables, and name the Pond\n"
    "  info                                 the Pond's id, link address, certificate and counts\n"
    "  name <name> [--description <text>]\n"
    "  channel create <name> [--topic <text>]\n"
    "  channel list\n"
    "  channel rename <name> <new name>\n"
    "  channel topic <name> <text>\n"
    "  channel delete <name>\n"
    "  invite create [--uses N] [--expires 7d|24h|90m|YYYY-MM-DD]\n"
    "  invite list\n"
    "  invite revoke <token>\n"
    "  member list\n"
    "  member remove <user id>\n";

static int usage()
{
    fputs(USAGE, stderr);
    return 2;
}

static int die(const std::string &why)
{
    fprintf(stderr, "pondctl: %s\n", why.c_str());
    return 1;
}

/* "7d", "24h", "90m", or a date (the end of that day, UTC). 0 if it is none of those. */
static int64_t parse_expiry(const std::string &s)
{
    char unit = s.empty() ? 0 : s.back();
    if (unit == 'd' || unit == 'h' || unit == 'm') {
        char *end = nullptr;
        long n = strtol(s.c_str(), &end, 10);
        if (n <= 0 || end != s.c_str() + s.size() - 1)
            return 0;
        int64_t per = unit == 'd' ? 86400000LL : unit == 'h' ? 3600000LL : 60000LL;
        return now_ms() + n * per;
    }
    struct tm tm = {};
    if (s.size() != 10 || !strptime(s.c_str(), "%Y-%m-%d", &tm))
        return 0;
    tm.tm_hour = 23, tm.tm_min = 59, tm.tm_sec = 59;
    time_t t = timegm(&tm);
    return t > 0 ? (int64_t)t * 1000 : 0;
}

static std::string when(int64_t ms)
{
    time_t t = (time_t)(ms / 1000);
    char buf[32];
    strftime(buf, sizeof buf, "%Y-%m-%d %H:%M UTC", gmtime(&t));
    return buf;
}

static std::string link_for(const Config &cfg, const std::string &token)
{
    std::string a = link_address(cfg);
    if (a.size() > 4 && a.compare(a.size() - 4, 4, ":443") == 0)
        a.resize(a.size() - 4);
    return "pond://" + a + "?invite=" + token;
}

/* `--name value` options after the positional words; false if one is unknown or has no value. */
static bool options(std::vector<std::string> &args, std::vector<std::pair<std::string, std::string *>> known)
{
    std::vector<std::string> rest;
    for (size_t i = 0; i < args.size(); i++) {
        if (args[i].rfind("--", 0) != 0) {
            rest.push_back(args[i]);
            continue;
        }
        bool found = false;
        for (auto &[name, out] : known)
            if (args[i] == name && i + 1 < args.size()) {
                *out = args[++i];
                found = true;
            }
        if (!found)
            return false;
    }
    args = rest;
    return true;
}

int main(int argc, char **argv)
{
    std::string path = "pond.yaml";
    std::vector<std::string> args;
    for (int i = 1; i < argc; i++) {
        if ((!strcmp(argv[i], "-c") || !strcmp(argv[i], "--config")) && i + 1 < argc)
            path = argv[++i];
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help"))
            return fputs(USAGE, stdout), 0;
        else
            args.push_back(argv[i]);
    }
    if (args.empty())
        return usage();
    Config cfg;
    std::string err;
    if (!load_config(path, cfg, err))
        return die(err);
    std::string cmd = args[0], sub = args.size() > 1 ? args[1] : "";
    args.erase(args.begin());

    PondKey key;
    if (cmd == "init") {
        if (args.size() != 1)
            return usage();
        if (!create_key(cfg.data_dir, key, err))
            return die(err);
    } else if (!load_key(cfg.data_dir, key, err)) {
        return die(err);
    }

    Pool pool(cfg.db, 4);
    {
        auto db = pool.get();
        if (!db)
            return die("cannot reach the database " + cfg.db.name);
        if (!migrate(*db, err))
            return die(err);
    }
    Service svc(pool, key);
    auto db = pool.get();

    if (cmd == "init") {
        std::string ignored;
        if (!svc.set_name(args[0], nullptr, err) || !svc.channel_create("general", "", ignored))
            return die(err);
        printf("Pond %s made, id %s, with a channel #general.\nMake an invite with: pondctl invite create\n", args[0].c_str(),
               key.id.c_str());
        return 0;
    }
    if (cmd == "info") {
        Result r;
        db->exec("SELECT (SELECT COUNT(*) FROM members WHERE left_ms IS NULL), (SELECT COUNT(*) FROM channels WHERE deleted = 0),"
                 " (SELECT COUNT(*) FROM messages)",
                 {}, &r);
        char e[256] = "", fp[65] = "";
        SSL_CTX *tls = puddle_tls_context(cfg.data_dir.c_str(), e, sizeof e);
        if (tls)
            puddle_tls_fingerprint_hex(tls, fp);
        SSL_CTX_free(tls);
        printf("name         %s\nid           %s\naddress      %s\ncertificate  %s\nmembers      %s\nchannels     %s\nmessages     %s\n",
               svc.setting("name").c_str(), key.id.c_str(), link_address(cfg).c_str(), *fp ? fp : e, text(r.rows[0][0]).c_str(),
               text(r.rows[0][1]).c_str(), text(r.rows[0][2]).c_str());
        return 0;
    }
    if (cmd == "name") {
        std::string description = "\x01";
        if (!options(args, {{"--description", &description}}) || args.size() != 1)
            return usage();
        bool has = description != "\x01";
        return svc.set_name(args[0], has ? &description : nullptr, err) ? 0 : die(err);
    }
    if (cmd == "channel") {
        args.erase(args.begin());
        std::string topic;
        if (!options(args, {{"--topic", &topic}}))
            return usage();
        bool ok;
        if (sub == "create" && args.size() == 1) {
            ok = svc.channel_create(args[0], topic, err);
        } else if (sub == "list" && args.empty()) {
            Result r;
            db->exec("SELECT name, id, topic FROM channels WHERE deleted = 0 ORDER BY position, id", {}, &r);
            for (auto &row : r.rows)
                printf("#%-24s %s  %s\n", text(row[0]).c_str(), text(row[1]).c_str(), text(row[2]).c_str());
            return 0;
        } else if (sub == "rename" && args.size() == 2) {
            ok = svc.channel_rename(args[0], args[1], err);
        } else if (sub == "topic" && args.size() == 2) {
            ok = svc.channel_topic(args[0], args[1], err);
        } else if (sub == "delete" && args.size() == 1) {
            ok = svc.channel_delete(args[0], err);
        } else {
            return usage();
        }
        return ok ? 0 : die(err);
    }
    if (cmd == "invite") {
        args.erase(args.begin());
        std::string uses, expires;
        if (!options(args, {{"--uses", &uses}, {"--expires", &expires}}))
            return usage();
        if (sub == "create" && args.empty()) {
            int n = uses.empty() ? 0 : atoi(uses.c_str());
            int64_t until = expires.empty() ? 0 : parse_expiry(expires);
            if ((!uses.empty() && n <= 0) || (!expires.empty() && until <= now_ms()))
                return die("--uses is a number above 0; --expires is like 7d, 24h, 90m or a date to come (YYYY-MM-DD)");
            std::string token;
            if (!svc.invite_create(n, until, token, err))
                return die(err);
            printf("%s\n", link_for(cfg, token).c_str());
            fprintf(stderr, "%s, %s\n", n ? (std::to_string(n) + (n == 1 ? " use" : " uses")).c_str() : "unlimited uses",
                    until ? ("expires " + when(until)).c_str() : "never expires");
            return 0;
        }
        if (sub == "list" && args.empty()) {
            Result r;
            db->exec("SELECT token, uses, max_uses, expires_ms, revoked FROM invites ORDER BY created_ms", {}, &r);
            int64_t now = now_ms();
            for (auto &row : r.rows) {
                std::string state = num(row[4])                             ? "revoked"
                                    : row[3] && num(row[3]) <= now          ? "expired"
                                    : row[2] && num(row[1]) >= num(row[2]) ? "used up"
                                                                            : "open";
                printf("%s  %-8s %s/%s uses  %s\n", text(row[0]).c_str(), state.c_str(), text(row[1]).c_str(),
                       row[2] ? row[2]->c_str() : "unlimited", row[3] ? ("until " + when(num(row[3]))).c_str() : "no expiry");
            }
            return 0;
        }
        if (sub == "revoke" && args.size() == 1)
            return svc.invite_revoke(args[0], err) ? 0 : die(err);
        return usage();
    }
    if (cmd == "member") {
        args.erase(args.begin());
        if (sub == "list" && args.empty()) {
            Result r;
            db->exec("SELECT id, JSON_VALUE(profile, '$.display_name'), joined_ms FROM members WHERE left_ms IS NULL ORDER BY joined_ms", {},
                     &r);
            for (auto &row : r.rows)
                printf("%s  %-24s joined %s\n", text(row[0]).c_str(), row[1] ? row[1]->c_str() : "(no profile)", when(num(row[2])).c_str());
            return 0;
        }
        if (sub == "remove" && args.size() == 1)
            return svc.member_remove(args[0], err) ? 0 : die(err);
        return usage();
    }
    return usage();
}
