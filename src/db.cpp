#include "db.h"

#include "base/log.h"

#include <mysql.h>
#include <errmsg.h>

#include <cstdlib>
#include <cstring>

namespace pond {

static std::once_flag library_once;

std::unique_ptr<Db> Db::connect(const DbConfig &cfg, std::string &err)
{
    std::call_once(library_once, [] { mysql_library_init(0, nullptr, nullptr); });
    std::unique_ptr<Db> d(new Db());
    d->cfg_ = cfg;
    if (!d->open(err))
        return nullptr;
    return d;
}

bool Db::open(std::string &err)
{
    if (my_)
        mysql_close(my_);
    my_ = mysql_init(nullptr);
    if (!my_) {
        err = "out of memory";
        return false;
    }
    unsigned timeout = 10;
    mysql_options(my_, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
    mysql_options(my_, MYSQL_SET_CHARSET_NAME, "utf8mb4");
    if (!mysql_real_connect(my_, cfg_.socket.empty() ? cfg_.host.c_str() : nullptr, cfg_.user.c_str(), cfg_.password.c_str(),
                            cfg_.name.c_str(), cfg_.socket.empty() ? cfg_.port : 0,
                            cfg_.socket.empty() ? nullptr : cfg_.socket.c_str(), 0)) {
        err = std::string("database: ") + mysql_error(my_);
        mysql_close(my_);
        my_ = nullptr;
        return false;
    }
    /* times are kept as milliseconds in BIGINTs, so the session's time zone never matters; strict mode makes a value that
     * does not fit an error rather than a quiet truncation */
    if (mysql_query(my_, "SET SESSION sql_mode = 'STRICT_ALL_TABLES,NO_ZERO_DATE,NO_ENGINE_SUBSTITUTION'") != 0) {
        err = std::string("database: ") + mysql_error(my_);
        return false;
    }
    return true;
}

Db::~Db()
{
    if (my_)
        mysql_close(my_);
}

/* `sql` with each `?` outside a quoted string replaced by the next parameter, escaped for this connection. */
static bool fill(MYSQL *my, const std::string &sql, const std::vector<Value> &params, std::string &out, std::string &err)
{
    size_t next = 0;
    char quote = 0;
    for (size_t i = 0; i < sql.size(); i++) {
        char c = sql[i];
        if (quote) {
            out += c;
            if (c == quote)
                quote = 0;
            continue;
        }
        if (c == '\'' || c == '"' || c == '`') {
            quote = c;
            out += c;
            continue;
        }
        if (c != '?') {
            out += c;
            continue;
        }
        if (next >= params.size()) {
            err = "too few parameters for: " + sql;
            return false;
        }
        const Value &v = params[next++];
        if (std::holds_alternative<std::nullptr_t>(v)) {
            out += "NULL";
        } else if (auto *n = std::get_if<int64_t>(&v)) {
            out += std::to_string(*n);
        } else {
            const std::string &s = std::get<std::string>(v);
            std::string esc(s.size() * 2 + 1, '\0');
            unsigned long len = mysql_real_escape_string(my, esc.data(), s.data(), (unsigned long)s.size());
            out += '\'';
            out.append(esc.data(), len);
            out += '\'';
        }
    }
    if (next != params.size()) {
        err = "too many parameters for: " + sql;
        return false;
    }
    return true;
}

bool Db::exec(const std::string &sql, const std::vector<Value> &params, Result *out, std::string &err)
{
    errno_ = 0;
    for (int attempt = 0; attempt < 2; attempt++) {
        if (!my_ && !open(err))
            return false;
        std::string q;
        if (!fill(my_, sql, params, q, err))
            return false;
        if (mysql_real_query(my_, q.data(), (unsigned long)q.size()) != 0) {
            errno_ = mysql_errno(my_);
            err = mysql_error(my_);
            if (attempt == 0 && (errno_ == CR_SERVER_GONE_ERROR || errno_ == CR_SERVER_LOST)) {
                mysql_close(my_);
                my_ = nullptr;
                continue;
            }
            return false;
        }
        MYSQL_RES *res = mysql_store_result(my_);
        if (out) {
            out->rows.clear();
            out->affected = mysql_affected_rows(my_);
            out->insert_id = (int64_t)mysql_insert_id(my_);
        }
        if (res) {
            unsigned n = mysql_num_fields(res);
            while (MYSQL_ROW row = mysql_fetch_row(res)) {
                if (!out)
                    continue;
                unsigned long *lens = mysql_fetch_lengths(res);
                std::vector<Cell> cells;
                for (unsigned i = 0; i < n; i++)
                    cells.push_back(row[i] ? Cell(std::string(row[i], lens[i])) : std::nullopt);
                out->rows.push_back(std::move(cells));
            }
            mysql_free_result(res);
        } else if (mysql_field_count(my_) != 0) {
            errno_ = mysql_errno(my_);
            err = mysql_error(my_);
            return false;
        }
        return true;
    }
    return false;
}

bool Db::exec(const std::string &sql, const std::vector<Value> &params, Result *out)
{
    std::string err;
    if (exec(sql, params, out, err))
        return true;
    if (errno_ != 1062) /* a duplicate is an answer the caller expects, not a fault */
        puddle_log(PUDDLE_LOG_ERROR, "database: %s", err.c_str());
    return false;
}

Pool::Lease Pool::get()
{
    std::unique_lock<std::mutex> lk(mu_);
    cv_.wait(lk, [&] { return !idle_.empty() || out_ < max_; });
    if (!idle_.empty()) {
        auto d = std::move(idle_.back());
        idle_.pop_back();
        out_++;
        return Lease(this, std::move(d));
    }
    out_++;
    lk.unlock();
    std::string err;
    auto d = Db::connect(cfg_, err);
    if (!d) {
        puddle_log(PUDDLE_LOG_ERROR, "%s", err.c_str());
        std::lock_guard<std::mutex> g(mu_);
        out_--;
        cv_.notify_one();
        return Lease(this, nullptr);
    }
    return Lease(this, std::move(d));
}

void Pool::give_back(std::unique_ptr<Db> d)
{
    std::lock_guard<std::mutex> g(mu_);
    out_--;
    idle_.push_back(std::move(d));
    cv_.notify_one();
}

int64_t num(const Cell &c)
{
    return c ? strtoll(c->c_str(), nullptr, 10) : 0;
}

std::string text(const Cell &c)
{
    return c ? *c : std::string();
}

/* Each step brings the schema from version i to i + 1. Never edit one that has shipped: add another. */
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
};

bool migrate(Db &db, std::string &err)
{
    Result r;
    if (!db.exec("CREATE TABLE IF NOT EXISTS schema_version (version INT NOT NULL) ENGINE=InnoDB", {}, nullptr, err) ||
        !db.exec("SELECT version FROM schema_version", {}, &r, err))
        return false;
    int64_t have = r.rows.empty() ? 0 : num(r.rows[0][0]);
    if (r.rows.empty() && !db.exec("INSERT INTO schema_version VALUES (0)", {}, nullptr, err))
        return false;
    int64_t want = (int64_t)(sizeof STEPS / sizeof STEPS[0]);
    if (have > want) {
        err = "the database is from a newer Pond (schema " + std::to_string(have) + ")";
        return false;
    }
    for (int64_t v = have; v < want; v++) {
        /* MariaDB commits each CREATE TABLE by itself, so a step that fails part way is finished by hand; each is small */
        for (const char *const *s = STEPS[v]; *s; s++)
            if (!db.exec(*s, {}, nullptr, err))
                return false;
        if (!db.exec("UPDATE schema_version SET version = ?", {v + 1}, nullptr, err))
            return false;
    }
    return true;
}

} // namespace pond
