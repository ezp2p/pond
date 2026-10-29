#include "db.h"

#include <drogon/orm/Exception.h>

#include <trantor/utils/Logger.h>

namespace pond {

using drogon::orm::DrogonDbException;

/* Each step brings the schema up one version. MariaDB commits each CREATE TABLE by itself, so a step that fails part way is
 * finished by hand; each is small. */
static const char *const STEPS[][8] = {
    {
        "CREATE TABLE settings (name VARCHAR(64) PRIMARY KEY, value TEXT NOT NULL) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
        "CREATE TABLE channels (id BIGINT AUTO_INCREMENT PRIMARY KEY, name VARCHAR(32) NOT NULL, topic VARCHAR(1024) NOT NULL DEFAULT '',"
        " position INT NOT NULL DEFAULT 0, created_ms BIGINT NOT NULL, deleted TINYINT NOT NULL DEFAULT 0,"
        " live_name VARCHAR(32) AS (IF(deleted = 0, name, NULL)) PERSISTENT, UNIQUE KEY (live_name)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
        "CREATE TABLE members (id CHAR(52) PRIMARY KEY, sign_pub CHAR(64) NOT NULL, kid CHAR(64) NOT NULL, seq BIGINT NOT NULL,"
        " chain MEDIUMTEXT NOT NULL, profile MEDIUMTEXT NULL, joined_ms BIGINT NOT NULL, left_ms BIGINT NULL) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
        "CREATE TABLE invites (token CHAR(26) PRIMARY KEY, max_uses INT NULL, uses INT NOT NULL DEFAULT 0, expires_ms BIGINT NULL,"
        " created_ms BIGINT NOT NULL, revoked TINYINT NOT NULL DEFAULT 0) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
        "CREATE TABLE messages (id BIGINT AUTO_INCREMENT PRIMARY KEY, channel BIGINT NOT NULL, author CHAR(52) NOT NULL,"
        " text TEXT NOT NULL, nonce VARCHAR(32) NULL, ts_ms BIGINT NOT NULL, KEY (channel, id), UNIQUE KEY (author, nonce))"
        " ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
        "CREATE TABLE events (id BIGINT AUTO_INCREMENT PRIMARY KEY, kind VARCHAR(32) NOT NULL, data MEDIUMTEXT NOT NULL,"
        " ts_ms BIGINT NOT NULL, KEY (ts_ms)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
        nullptr,
    },
    {
        /* add_event() locks this row until its transaction commits, so events commit in the order of their ids: a member
         * whose cursor has passed an id has seen everything before it */
        "INSERT IGNORE INTO settings (name, value) VALUES ('event_lock', '')",
        nullptr,
    },
    {
        /* files (docs/design.md, "Files"): the bytes are in data_dir/files; `have` is a '0' or '1' for each chunk */
        "CREATE TABLE files (id CHAR(64) PRIMARY KEY, size BIGINT NOT NULL, chunks MEDIUMBLOB NOT NULL, have MEDIUMBLOB NOT NULL,"
        " whole TINYINT NOT NULL DEFAULT 0, uploader CHAR(52) NOT NULL, created_ms BIGINT NOT NULL, KEY (whole, created_ms))"
        " ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
        "CREATE TABLE message_files (message BIGINT NOT NULL, position TINYINT NOT NULL, file CHAR(64) NOT NULL,"
        " name VARCHAR(255) NOT NULL, mime VARCHAR(127) NOT NULL, size BIGINT NOT NULL, w INT NULL, h INT NULL,"
        " duration_ms BIGINT NULL, preview TEXT NULL, PRIMARY KEY (message, position), KEY (file)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
        nullptr,
    },
    {
        /* roles (docs/design.md, "Roles"), offers of them, bans, the admin log, and events for one member */
        "ALTER TABLE members ADD COLUMN role VARCHAR(16) NOT NULL DEFAULT 'member'",
        "ALTER TABLE events ADD COLUMN only_for CHAR(52) NULL",
        "CREATE TABLE role_offers (id BIGINT AUTO_INCREMENT PRIMARY KEY, member CHAR(52) NOT NULL, role VARCHAR(16) NOT NULL,"
        " created_ms BIGINT NOT NULL, KEY (member)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
        "CREATE TABLE bans (member CHAR(52) PRIMARY KEY, by_who VARCHAR(52) NOT NULL, reason VARCHAR(500) NOT NULL DEFAULT '',"
        " ts_ms BIGINT NOT NULL) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
        "CREATE TABLE admin_log (id BIGINT AUTO_INCREMENT PRIMARY KEY, by_who VARCHAR(52) NOT NULL, action VARCHAR(32) NOT NULL,"
        " target VARCHAR(128) NOT NULL DEFAULT '', detail TEXT NULL, ts_ms BIGINT NOT NULL, KEY (ts_ms)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
        nullptr,
    },
};

/* A password in a connection string: quoted, with backslashes and quotes escaped. Drogon's parser takes a quoted value to run
 * to the last quote in the string, so only the password is quoted, and it goes last. */
static std::string quoted(const std::string &v)
{
    std::string o = "'";
    for (char c : v) {
        if (c == '\\' || c == '\'')
            o += '\\';
        o += c;
    }
    return o + "'";
}

DbPtr connect_db(const DbConfig &cfg)
{
    std::string info = "host=" + cfg.host + " port=" + std::to_string(cfg.port) + " dbname=" + cfg.name + " user=" + cfg.user +
                       " client_encoding=utf8mb4";
    if (!cfg.password.empty())
        info += " password=" + quoted(cfg.password);
    DbPtr db = drogon::orm::DbClient::newMysqlClient(info, cfg.connections);
    db->setTimeout(10); /* a statement that waits longer than this for a connection fails rather than hanging */
    return db;
}

bool migrate(const DbPtr &db, std::string &err)
{
    try {
        db->execSqlSync("CREATE TABLE IF NOT EXISTS schema_version (version INT NOT NULL) ENGINE=InnoDB");
        auto r = db->execSqlSync("SELECT version FROM schema_version");
        int64_t have = r.empty() ? 0 : num(r[0][0]);
        if (r.empty())
            db->execSqlSync("INSERT INTO schema_version VALUES (0)");
        int64_t want = (int64_t)(sizeof STEPS / sizeof STEPS[0]);
        if (have > want) {
            err = "the database is from a newer Pond (schema " + std::to_string(have) + ")";
            return false;
        }
        for (int64_t v = have; v < want; v++) {
            for (const char *const *s = STEPS[v]; *s; s++)
                db->execSqlSync(*s);
            db->execSqlSync("UPDATE schema_version SET version = ?", v + 1);
        }
        return true;
    } catch (const DrogonDbException &e) {
        err = std::string("database: ") + e.base().what();
        return false;
    }
}

int64_t num(const drogon::orm::Field &f)
{
    return f.isNull() ? 0 : f.as<int64_t>();
}

std::string text(const drogon::orm::Field &f)
{
    return f.isNull() ? "" : f.as<std::string>();
}

int sql_errno(const DrogonDbException &e)
{
    auto *s = dynamic_cast<const drogon::orm::SqlError *>(&e.base());
    return s ? s->errcode() : 0;
}

} // namespace pond
