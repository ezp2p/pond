/* pondctl: the Pond admin's tool (docs/design.md, "pondctl"). It works on the database and data folder pondd uses, so it runs
 * beside it on the same machine; pondd notices its changes within half a second. */

#include "config.h"
#include "db.h"
#include "encoding.h"
#include "key.h"
#include "pond.h"
#include "tls.h"

#include <drogon/orm/Exception.h>
#include <trantor/utils/Logger.h>

#include <cstdio>
#include <cstdlib>
#include "voice.h"

#include <cstring>
#include <ctime>
#include <fstream>
#include <string>
#include <vector>

using namespace pond;
using drogon::sync_wait;
using drogon::orm::Result;

static const char USAGE[] =
    "usage: pondctl [-c pond.yaml] <command>\n"
    "\n"
    "  init <name>                          make the key and the database tables, and name the Pond\n"
    "  info                                 the Pond's id, link address, certificate and counts\n"
    "  name <name> [--description <text>]\n"
    "  icon <picture file>|off             the Pond's icon: a PNG, JPEG, GIF or WebP of at most 1 MiB\n"
    "  channel create <name> [--voice] [--topic <text>]   --voice: a voice channel (docs/voice.md)\n"
    "  channel list\n"
    "  channel rename <name> <new name>\n"
    "  channel topic <name> <text>\n"
    "  channel delete <name>\n"
    "  invite create [--uses N] [--expires 7d|24h|90m|YYYY-MM-DD]\n"
    "  invite list\n"
    "  invite revoke <token>\n"
    "  member list\n"
    "  member offer <member> operator|administrator [--yes]   offer a role; they accept it in their app\n"
    "  member role <member> member|administrator              take a role away (or lower it) at once\n"
    "  member remove <member>\n"
    "  member ban <member> [--reason <text>]                  remove them, and refuse them if they join again\n"
    "  member unban <user id>\n"
    "  member bans\n"
    "  log [N]                                                the last N admin actions (20)\n"
    "  message show <id>                    a message, with every text it had, and where its files are\n"
    "  message delete <id>                  take it out of the channel; it stays in the message log\n"
    "  message purge <id> [--yes]           erase it for good, and its files that nothing else carries\n"
    "    <member> is a user id, or part of a member's display name\n"
    "  files                                the largest file members may send, and the room files take\n"
    "  files max-size <size>|off            e.g. 25M, 1G, 500K; off stops members sending files\n"
    "  dms                                  whether members may write to each other here, and the largest file\n"
    "  dms on|off                           direct messages between members (relayed, never kept)\n"
    "  dms files max-size <size>|off        the largest file a direct message may carry\n";

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

/* "25M", "1G", "500K", "1048576", or "off" (0). -1 if it is none of those. */
static int64_t parse_size(const std::string &s)
{
    if (s == "off")
        return 0;
    char *end = nullptr;
    double n = strtod(s.c_str(), &end);
    if (end == s.c_str() || n < 0)
        return -1;
    std::string unit = end;
    for (auto &c : unit)
        c = (char)toupper((unsigned char)c);
    if (unit.size() == 2 && unit[1] == 'B')
        unit.resize(1);
    int64_t per = unit.empty() || unit == "B" ? 1 : unit == "K" ? 1 << 10 : unit == "M" ? 1 << 20 : unit == "G" ? 1 << 30 : -1;
    return per < 0 ? -1 : (int64_t)(n * per);
}

static std::string size_text(int64_t b)
{
    char buf[32];
    if (b >= 1 << 30)
        snprintf(buf, sizeof buf, "%.1f GiB", b / 1073741824.0);
    else if (b >= 1 << 20)
        snprintf(buf, sizeof buf, "%.1f MiB", b / 1048576.0);
    else
        snprintf(buf, sizeof buf, "%lld KiB", (long long)(b >> 10));
    return buf;
}

static std::string when(int64_t ms)
{
    time_t t = (time_t)(ms / 1000);
    char buf[32];
    strftime(buf, sizeof buf, "%Y-%m-%d %H:%M UTC", gmtime(&t));
    return buf;
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

static int run(const Config &cfg, Service &svc, const DbPtr &db, const PondKey &key, const std::string &cmd, const std::string &sub,
               std::vector<std::string> &args);

/* A current member: `who` is their user id, or part of their display name (any case) that only one member's has. Their id, display
 * name, role and when they joined; false with the reason in `why`. */
struct Who {
    std::string id, name, role;
    int64_t joined = 0;
};
static bool find_member(const DbPtr &db, const std::string &who, Who &out, std::string &why)
{
    Result r = db->execSqlSync("SELECT id, COALESCE(JSON_VALUE(profile, '$.display_name'), ''), role, joined_ms FROM members"
                               " WHERE left_ms IS NULL AND (id = ? OR LOWER(JSON_VALUE(profile, '$.display_name')) LIKE CONCAT('%', LOWER(?), '%'))",
                               who, who);
    for (const auto &row : r)
        if (text(row[0]) == who) /* an exact id wins */
            r = db->execSqlSync("SELECT id, COALESCE(JSON_VALUE(profile, '$.display_name'), ''), role, joined_ms FROM members WHERE id = ?", who);
    if (r.size() != 1) {
        why = r.empty() ? "no member is " + who : std::to_string(r.size()) + " members match " + who + "; give their user id (pondctl member list)";
        return false;
    }
    out = {text(r[0][0]), text(r[0][1]), text(r[0][2]), num(r[0][3])};
    return true;
}

/* Ask on the terminal; only "y" or "yes" is a yes. */
static bool confirm(const std::string &question)
{
    printf("%s [y/N] ", question.c_str());
    fflush(stdout);
    char line[16] = "";
    if (!fgets(line, sizeof line, stdin))
        return false;
    return !strcmp(line, "y\n") || !strcmp(line, "yes\n") || !strcmp(line, "Y\n");
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

    trantor::Logger::setLogLevel(trantor::Logger::kWarn);
    PondKey key;
    if (cmd == "init") {
        if (args.size() != 1)
            return usage();
        if (!create_key(cfg.data_dir, key, err) || !ensure_certificate(cfg.data_dir, err))
            return die(err);
    } else if (!load_key(cfg.data_dir, key, err)) {
        return die(err);
    }

    DbPtr db = connect_db(cfg.db);
    if (!migrate(db, err))
        return die(err);
    Service svc(db, key, cfg.data_dir);
    /* invites carry the Pond's sealing key, `k` (spec/09, "First contact"): the join is sealed to it, so nothing in it can be
     * read on the way, whatever carries it */
    svc.link_address = link_address(cfg);
    svc.voice_on = cfg.voice.on && Voice::built();
    try {
        return run(cfg, svc, db, key, cmd, sub, args);
    } catch (const drogon::orm::DrogonDbException &e) {
        return die(std::string("database: ") + e.base().what());
    }
}

/* One command, once the key and database are ready. */
static int run(const Config &cfg, Service &svc, const DbPtr &db, const PondKey &key, const std::string &cmd, const std::string &sub,
               std::vector<std::string> &args)
{
    /* what an admin action said: "" is done, anything else is why not */
    auto done = [](const Outcome &o) { return o.empty() ? 0 : die(o); };
    if (cmd == "init") {
        Outcome o = sync_wait(svc.set_name(args[0], std::nullopt));
        if (!o.empty())
            return die(o);
        sync_wait(svc.channel_create("general", "")); /* already there when init is run again on the same database */
        printf("Pond %s made, id %s, with a channel #general.\nMake an invite with: pondctl invite create\n", args[0].c_str(),
               key.id.c_str());
        return 0;
    }
    if (cmd == "info") {
        Result r = db->execSqlSync("SELECT (SELECT COUNT(*) FROM members WHERE left_ms IS NULL),"
                                   " (SELECT COUNT(*) FROM channels WHERE deleted = 0), (SELECT COUNT(*) FROM messages)");
        printf("name         %s\nid           %s\nseal key     %s\naddress      %s\nmembers      %s\nchannels     %s\nmessages     %s\n",
               sync_wait(svc.setting("name")).c_str(), key.id.c_str(), hex(key.seal_pub, 32).c_str(), link_address(cfg).c_str(),
               text(r[0][0]).c_str(), text(r[0][1]).c_str(), text(r[0][2]).c_str());
        return 0;
    }
    if (cmd == "name") {
        std::string description = "\x01";
        if (!options(args, {{"--description", &description}}) || args.size() != 1)
            return usage();
        std::optional<std::string> d;
        if (description != "\x01")
            d = description;
        return done(sync_wait(svc.set_name(args[0], d)));
    }
    if (cmd == "icon") {
        if (args.size() != 1)
            return usage();
        if (args[0] == "off")
            return done(sync_wait(svc.set_icon(std::nullopt)));
        std::ifstream in(args[0], std::ios::binary);
        std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()), hash;
        if (!in.good() && !in.eof())
            return die("cannot read " + args[0]);
        Outcome o = svc.store_picture(bytes, hash);
        if (!o.empty())
            return die(o);
        Outcome set = sync_wait(svc.set_icon(hash));
        if (set.empty())
            sync_wait(svc.log_admin("pondctl", "set_icon", "", hash));
        return done(set);
    }
    if (cmd == "channel") {
        args.erase(args.begin());
        std::string topic;
        bool voice = std::erase(args, std::string("--voice")) > 0;
        if (!options(args, {{"--topic", &topic}}))
            return usage();
        if (sub == "create" && args.size() == 1)
            return done(sync_wait(svc.channel_create(args[0], topic, voice ? "voice" : "text")));
        if (voice)
            return usage();
        if (sub == "list" && args.empty()) {
            Result r = db->execSqlSync("SELECT name, id, topic, kind FROM channels WHERE deleted = 0 ORDER BY position, id");
            for (const auto &row : r)
                printf("#%-24s %s  %-5s  %s\n", text(row[0]).c_str(), text(row[1]).c_str(), text(row[3]).c_str(), text(row[2]).c_str());
            return 0;
        }
        if (sub == "rename" && args.size() == 2)
            return done(sync_wait(svc.channel_rename(args[0], args[1])));
        if (sub == "topic" && args.size() == 2)
            return done(sync_wait(svc.channel_topic(args[0], args[1])));
        if (sub == "delete" && args.size() == 1)
            return done(sync_wait(svc.channel_delete(args[0])));
        return usage();
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
            auto token = std::make_shared<std::string>();
            Outcome o = sync_wait(svc.invite_create(n, until, token));
            if (!o.empty())
                return die(o);
            printf("%s\n", svc.invite_link(*token).c_str());
            fprintf(stderr, "%s, %s\n", n ? (std::to_string(n) + (n == 1 ? " use" : " uses")).c_str() : "unlimited uses",
                    until ? ("expires " + when(until)).c_str() : "never expires");
            return 0;
        }
        if (sub == "list" && args.empty()) {
            Result r = db->execSqlSync("SELECT token, uses, max_uses, expires_ms, revoked FROM invites ORDER BY created_ms");
            int64_t now = now_ms();
            for (const auto &row : r) {
                std::string state = num(row[4])                                    ? "revoked"
                                    : !row[3].isNull() && num(row[3]) <= now        ? "expired"
                                    : !row[2].isNull() && num(row[1]) >= num(row[2]) ? "used up"
                                                                                     : "open";
                printf("%s  %-8s %s/%s uses  %s\n", text(row[0]).c_str(), state.c_str(), text(row[1]).c_str(),
                       row[2].isNull() ? "unlimited" : text(row[2]).c_str(), row[3].isNull() ? "no expiry" : ("until " + when(num(row[3]))).c_str());
            }
            return 0;
        }
        if (sub == "revoke" && args.size() == 1)
            return done(sync_wait(svc.invite_revoke(args[0])));
        return usage();
    }
    if (cmd == "files") {
        if (!args.empty())
            args.erase(args.begin());
        if (sub.empty() && args.empty()) {
            int64_t max = sync_wait(svc.files_max());
            auto [count, bytes] = sync_wait(svc.files_usage());
            printf("largest file  %s\nkept         %lld file(s), %s\n", max ? size_text(max).c_str() : "off (members cannot send files)",
                   (long long)count, size_text(bytes).c_str());
            return 0;
        }
        if (sub == "max-size" && args.size() == 1) {
            int64_t b = parse_size(args[0]);
            if (b < 0)
                return die("a size is like 25M, 1G or 500K, or off");
            return done(sync_wait(svc.set_files_max(b)));
        }
        return usage();
    }
    if (cmd == "dms") {
        if (!args.empty())
            args.erase(args.begin());
        if (sub.empty() && args.empty()) {
            Service::DmSettings d = sync_wait(svc.dm_settings());
            printf("direct messages  %s\nlargest file     %s\n", d.on ? "on" : "off",
                   d.files_max ? size_text(d.files_max).c_str() : "off (no files in direct messages)");
            return 0;
        }
        if ((sub == "on" || sub == "off") && args.empty())
            return done(sync_wait(svc.set_dms(sub == "on", std::nullopt)));
        if (sub == "files" && args.size() == 2 && args[0] == "max-size") {
            int64_t b = parse_size(args[1]);
            if (b < 0)
                return die("a size is like 25M, 1G or 500K, or off");
            return done(sync_wait(svc.set_dms(std::nullopt, b)));
        }
        return usage();
    }
    if (cmd == "member") {
        if (!args.empty())
            args.erase(args.begin());
        std::string reason;
        bool flag_yes = false;
        for (auto it = args.begin(); it != args.end();)
            if (*it == "--yes") {
                flag_yes = true;
                it = args.erase(it);
            } else {
                ++it;
            }
        if (!options(args, {{"--reason", &reason}}))
            return usage();
        if (sub == "list" && args.empty()) {
            Result r = db->execSqlSync("SELECT id, JSON_VALUE(profile, '$.display_name'), joined_ms, role FROM members WHERE left_ms IS NULL"
                                       " ORDER BY FIELD(role, 'operator', 'administrator', 'member'), joined_ms");
            for (const auto &row : r)
                printf("%s  %-24s %-14s joined %s\n", text(row[0]).c_str(), row[1].isNull() ? "(no profile)" : text(row[1]).c_str(),
                       text(row[3]).c_str(), when(num(row[2])).c_str());
            return 0;
        }
        if (sub == "bans" && args.empty()) {
            Result r = db->execSqlSync("SELECT b.member, JSON_VALUE(m.profile, '$.display_name'), b.by_who, b.reason, b.ts_ms FROM bans b"
                                       " LEFT JOIN members m ON m.id = b.member ORDER BY b.ts_ms");
            for (const auto &row : r)
                printf("%s  %-24s banned %s by %s%s\n", text(row[0]).c_str(), row[1].isNull() ? "(no profile)" : text(row[1]).c_str(),
                       when(num(row[4])).c_str(), text(row[2]).substr(0, 12).c_str(), text(row[3]).empty() ? "" : (": " + text(row[3])).c_str());
            return 0;
        }
        if (sub == "unban" && args.size() == 1)
            return done(sync_wait(svc.unban(args[0], "pondctl")));
        Who w;
        std::string why;
        if ((sub == "offer" || sub == "role") && args.size() == 2) {
            auto role = role_of_name(args[1]);
            if (!role || (sub == "offer" && role == Role::MEMBER))
                return die(sub == "offer" ? "offer operator or administrator" : "a role is member, administrator or operator");
            if (!find_member(db, args[0], w, why))
                return die(why);
            auto now = role_of_name(w.role).value_or(Role::MEMBER);
            printf("%s\n  name    %s\n  role    %s\n  joined  %s\n", w.id.c_str(), w.name.empty() ? "(no profile)" : w.name.c_str(),
                   w.role.c_str(), when(w.joined).c_str());
            if (sub == "role" && *role > now)
                return die("a role is offered, not given: pondctl member offer " + w.id + " " + args[1]);
            if (*role == now && sub == "offer")
                return die((w.name.empty() ? w.id : w.name) + " is already " + w.role);
            std::string q = sub == "offer" ? "Offer " + args[1] + " to " + (w.name.empty() ? w.id.substr(0, 12) : w.name) + "?"
                                           : "Make " + (w.name.empty() ? w.id.substr(0, 12) : w.name) + " " + args[1] + "?";
            if (!flag_yes && !confirm(q))
                return die("nothing changed");
            if (sub == "offer") {
                Outcome o = sync_wait(svc.offer_role(w.id, *role));
                if (!o.empty())
                    return die(o);
                printf("Offered. They will see it in their app, and it lapses in 7 days unless they answer.\n");
                return 0;
            }
            return done(sync_wait(svc.set_role(w.id, *role, "pondctl")));
        }
        if ((sub == "remove" || sub == "ban") && args.size() == 1) {
            if (!find_member(db, args[0], w, why)) {
                if (sub == "ban" && args[0].size() == 52) /* someone who has left can still be banned, by id */
                    return done(sync_wait(svc.ban(args[0], reason, "pondctl")));
                return die(why);
            }
            if (sub == "remove") {
                Outcome o = sync_wait(svc.member_remove(w.id));
                if (o.empty())
                    sync_wait(svc.log_admin("pondctl", "remove", w.id, ""));
                return done(o);
            }
            return done(sync_wait(svc.ban(w.id, reason, "pondctl")));
        }
        return usage();
    }
    if (cmd == "message") {
        /* editing and deleting messages (docs/design.md, "Editing and deleting messages"): the id is the one in the app's
         * message log, or in `message show` */
        if (!args.empty())
            args.erase(args.begin());
        const bool flag_yes = std::erase(args, std::string("--yes")) > 0;
        const int64_t id = args.size() == 1 ? strtoll(args[0].c_str(), nullptr, 10) : 0;
        if (id <= 0)
            return usage();
        if (sub == "show") {
            auto out = std::make_shared<std::string>();
            Outcome o = sync_wait(svc.message_show(id, out));
            if (!o.empty())
                return die(o);
            fputs(out->c_str(), stdout);
            return 0;
        }
        if (sub == "delete")
            return done(sync_wait(svc.message_delete_ctl(id)));
        if (sub == "purge") {
            if (!flag_yes && !confirm("Erase message " + args[0] + " and its files for good? Nothing can bring them back."))
                return die("nothing changed");
            return done(sync_wait(svc.message_purge_ctl(id)));
        }
        return usage();
    }
    if (cmd == "log") {
        int n = sub.empty() ? 20 : atoi(sub.c_str());
        Result r = db->execSqlSync("SELECT ts_ms, by_who, action, target, detail FROM admin_log ORDER BY id DESC LIMIT ?", (int64_t)(n > 0 ? n : 20));
        for (auto row = r.rbegin(); row != r.rend(); ++row)
            printf("%s  %-12s %-15s %s%s\n", when(num((*row)[0])).c_str(), text((*row)[1]).substr(0, 12).c_str(), text((*row)[2]).c_str(),
                   text((*row)[3]).substr(0, 16).c_str(), text((*row)[4]).empty() ? "" : ("  " + text((*row)[4])).c_str());
        return 0;
    }
    return usage();
}
