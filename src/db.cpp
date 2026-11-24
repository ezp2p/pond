#include "db.h"

#include <drogon/orm/Exception.h>

#include <trantor/utils/Logger.h>

namespace pond {

using drogon::orm::DrogonDbException;

/* Each step brings the schema up one version. MariaDB commits each CREATE TABLE by itself, so a step that fails part way is
 * finished by hand; each is small. */
static const char *const STEPS[][16] = {
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
    {
        /* voice channels (docs/voice.md): who is in one is kept in pondd's memory, not here */
        "ALTER TABLE channels ADD COLUMN kind VARCHAR(8) NOT NULL DEFAULT 'text'",
        nullptr,
    },
    {
        /* muting (docs/design.md, "Roles"): NULL is not muted, 0 muted until unmuted, else muted until then (ms) */
        "ALTER TABLE members ADD COLUMN muted_until BIGINT NULL",
        nullptr,
    },
    {
        /* direct messages (docs/dms.md): whether a member takes them here. The letters themselves are never kept. */
        "ALTER TABLE members ADD COLUMN dms TINYINT NOT NULL DEFAULT 1",
        nullptr,
    },
    {
        /* roles for channels (docs/design.md, "Roles for channels"): named by the admins, held by members, and asked for by a
         * channel that is not open to everyone (`restricted`). An event about one channel says which, so members who may not
         * see it are not given it. */
        "CREATE TABLE roles (id BIGINT AUTO_INCREMENT PRIMARY KEY, name VARCHAR(32) NOT NULL, created_ms BIGINT NOT NULL,"
        " UNIQUE KEY (name)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
        "CREATE TABLE member_roles (member CHAR(52) NOT NULL, role BIGINT NOT NULL, PRIMARY KEY (member, role), KEY (role))"
        " ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
        "CREATE TABLE channel_roles (channel BIGINT NOT NULL, role BIGINT NOT NULL, PRIMARY KEY (channel, role), KEY (role))"
        " ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
        "ALTER TABLE channels ADD COLUMN restricted TINYINT NOT NULL DEFAULT 0",
        "ALTER TABLE events ADD COLUMN channel BIGINT NULL",
        nullptr,
    },
    {
        /* what roles let their members do, their colour, their order, and whether they are shown apart (docs/design.md,
         * "Permissions"): `permissions` is bits, `color` "#rrggbb" or "" for none, and the lower the `position` the higher */
        "ALTER TABLE roles ADD COLUMN permissions BIGINT NOT NULL DEFAULT 0, ADD COLUMN color VARCHAR(7) NOT NULL DEFAULT '',"
        " ADD COLUMN hoist TINYINT NOT NULL DEFAULT 0, ADD COLUMN position BIGINT NOT NULL DEFAULT 0",
        "UPDATE roles SET position = id",
        nullptr,
    },
    {
        /* pages (docs/design.md, "Pages"): shown above the channels, in their order; markdown, and the files they carry. The
         * banner is a setting, as the icon is. */
        "CREATE TABLE pages (id BIGINT AUTO_INCREMENT PRIMARY KEY, title VARCHAR(64) NOT NULL, kind VARCHAR(16) NOT NULL DEFAULT 'markdown',"
        " body MEDIUMTEXT NOT NULL, position BIGINT NOT NULL DEFAULT 0, created_ms BIGINT NOT NULL, updated_ms BIGINT NOT NULL,"
        " updated_by CHAR(52) NOT NULL) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
        "CREATE TABLE page_files (page BIGINT NOT NULL, position TINYINT NOT NULL, file CHAR(64) NOT NULL, name VARCHAR(255) NOT NULL,"
        " mime VARCHAR(127) NOT NULL, size BIGINT NOT NULL, w INT NULL, h INT NULL, PRIMARY KEY (page, position), KEY (file))"
        " ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
        nullptr,
    },
    {
        /* role colours (docs/design.md, "Role colours"): whether a member's name shows in their role's colour; the Pond's
         * choice to make everyone's do is a setting */
        "ALTER TABLE members ADD COLUMN role_color TINYINT NOT NULL DEFAULT 0",
        nullptr,
    },
    {
        /* protocol 13 (docs/design.md). Permissions for each channel: what it allows and denies a role, or everyone (role 0).
         * A channel only some roles saw becomes one that denies everyone `view_channels` (256) and allows it those roles. */
        "CREATE TABLE channel_overrides (channel BIGINT NOT NULL, role BIGINT NOT NULL, allow BIGINT NOT NULL DEFAULT 0,"
        " deny BIGINT NOT NULL DEFAULT 0, PRIMARY KEY (channel, role)) ENGINE=InnoDB",
        "INSERT INTO channel_overrides (channel, role, allow, deny) SELECT id, 0, 0, 256 FROM channels WHERE restricted = 1",
        "INSERT INTO channel_overrides (channel, role, allow, deny) SELECT channel, role, 256, 0 FROM channel_roles",
        /* pages: for some roles only, and each kind's settings */
        "ALTER TABLE pages ADD COLUMN restricted TINYINT NOT NULL DEFAULT 0, ADD COLUMN data MEDIUMTEXT NULL",
        "CREATE TABLE page_roles (page BIGINT NOT NULL, role BIGINT NOT NULL, PRIMARY KEY (page, role)) ENGINE=InnoDB",
        /* events on an events page, and who is going */
        "CREATE TABLE page_events (id BIGINT AUTO_INCREMENT PRIMARY KEY, page BIGINT NOT NULL, title VARCHAR(100) NOT NULL,"
        " description TEXT NOT NULL, place VARCHAR(200) NOT NULL DEFAULT '', starts_ms BIGINT NOT NULL, ends_ms BIGINT NULL,"
        " created_by CHAR(52) NOT NULL, created_ms BIGINT NOT NULL, KEY (page, starts_ms)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
        "CREATE TABLE event_going (event BIGINT NOT NULL, member CHAR(52) NOT NULL, PRIMARY KEY (event, member)) ENGINE=InnoDB",
        /* applications to join, while a Pond has an entry form */
        "CREATE TABLE applications (member CHAR(52) PRIMARY KEY, sign_pub CHAR(64) NOT NULL, kid VARCHAR(64) NOT NULL, seq BIGINT NOT NULL,"
        " chain MEDIUMTEXT NOT NULL, profile MEDIUMTEXT NULL, invite VARCHAR(32) NOT NULL, answers MEDIUMTEXT NOT NULL,"
        " status VARCHAR(16) NOT NULL, reason VARCHAR(500) NOT NULL DEFAULT '', created_ms BIGINT NOT NULL,"
        " decided_by CHAR(52) NULL, decided_ms BIGINT NULL, KEY (status, created_ms)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
        /* the shop: what a shop page sells, and orders, paid through Stripe */
        "CREATE TABLE shop_items (id BIGINT AUTO_INCREMENT PRIMARY KEY, page BIGINT NOT NULL, title VARCHAR(100) NOT NULL,"
        " description TEXT NOT NULL, image VARCHAR(255) NOT NULL DEFAULT '', price BIGINT NOT NULL, custom TINYINT NOT NULL DEFAULT 0,"
        " stock BIGINT NULL, active TINYINT NOT NULL DEFAULT 1, position BIGINT NOT NULL DEFAULT 0, KEY (page))"
        " ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
        "CREATE TABLE shop_orders (id BIGINT AUTO_INCREMENT PRIMARY KEY, item BIGINT NOT NULL, member CHAR(52) NOT NULL,"
        " title VARCHAR(100) NOT NULL, quantity BIGINT NOT NULL, amount BIGINT NOT NULL, currency CHAR(3) NOT NULL,"
        " status VARCHAR(16) NOT NULL, link_id VARCHAR(255) NOT NULL DEFAULT '', link_url VARCHAR(1024) NOT NULL DEFAULT '',"
        " live TINYINT NOT NULL DEFAULT 0, created_ms BIGINT NOT NULL, paid_ms BIGINT NULL, KEY (member), KEY (status, created_ms))"
        " ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
        nullptr,
    },
    {
        /* editing and deleting messages (docs/design.md, "Editing and deleting messages"): a deleted message stays, for the
         * message log, until it is purged; each earlier text of an edited one is a version; a file an edit dropped stays with
         * the message, marked; and the events about a message say which, so they can be rewritten when it changes */
        "ALTER TABLE messages ADD COLUMN edited_ms BIGINT NULL, ADD COLUMN deleted_ms BIGINT NULL, ADD COLUMN deleted_by VARCHAR(52) NULL",
        "ALTER TABLE message_files ADD COLUMN removed_ms BIGINT NULL",
        "CREATE TABLE message_versions (id BIGINT AUTO_INCREMENT PRIMARY KEY, message BIGINT NOT NULL, text TEXT NOT NULL,"
        " ts_ms BIGINT NOT NULL, until_ms BIGINT NOT NULL, KEY (message)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
        "ALTER TABLE events ADD COLUMN message BIGINT NULL, ADD KEY (message)",
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
