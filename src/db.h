#pragma once

/* MariaDB, as the Pond uses it: SQL with `?` placeholders, filled in escaped, and connections borrowed from a small pool so
 * each thread has one of its own while it works. The schema is in db.cpp (`migrate`). */

#include "config.h"

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <variant>
#include <vector>

typedef struct st_mysql MYSQL;

namespace pond {

using Value = std::variant<std::nullptr_t, int64_t, std::string>;
using Cell = std::optional<std::string>; /* nullopt is SQL NULL */

struct Result {
    std::vector<std::vector<Cell>> rows;
    uint64_t affected = 0;
    int64_t insert_id = 0;
};

/* One connection. Not for use by two threads at once: borrow one from a Pool. */
class Db {
public:
    static std::unique_ptr<Db> connect(const DbConfig &cfg, std::string &err);
    ~Db();

    /* Run one statement. Each `?` outside quotes takes the next of `params`. false with the reason in `err`, and the server's
     * error number in `errno_` (1062 is a duplicate key). A lost connection is made again and the statement retried once. */
    bool exec(const std::string &sql, const std::vector<Value> &params, Result *out, std::string &err);
    bool exec(const std::string &sql, const std::vector<Value> &params = {}, Result *out = nullptr);
    unsigned last_errno() const { return errno_; }

    /* Statements run between begin() and commit() happen all together or not at all. rollback() is safe to call twice. */
    bool begin() { return exec("START TRANSACTION"); }
    bool commit() { return exec("COMMIT"); }
    void rollback() { exec("ROLLBACK"); }

private:
    Db() = default;
    bool open(std::string &err);
    MYSQL *my_ = nullptr;
    DbConfig cfg_;
    unsigned errno_ = 0;
};

class Pool {
public:
    explicit Pool(DbConfig cfg, size_t max = 32) : cfg_(std::move(cfg)), max_(max) {}

    class Lease {
    public:
        Lease(Pool *p, std::unique_ptr<Db> d) : pool_(p), db_(std::move(d)) {}
        Lease(Lease &&) = default;
        ~Lease() { if (db_) pool_->give_back(std::move(db_)); }
        Db *operator->() { return db_.get(); }
        Db &operator*() { return *db_; }
        explicit operator bool() const { return db_ != nullptr; }
    private:
        Pool *pool_;
        std::unique_ptr<Db> db_;
    };

    /* A connection for this thread until the Lease goes away. Waits while `max` are out. An empty Lease if none can be made. */
    Lease get();

private:
    void give_back(std::unique_ptr<Db> d);
    DbConfig cfg_;
    size_t max_, out_ = 0;
    std::vector<std::unique_ptr<Db>> idle_;
    std::mutex mu_;
    std::condition_variable cv_;
};

/* Make or bring up to date the Pond's tables. false with the reason in `err`. */
bool migrate(Db &db, std::string &err);

/* A cell as a number (0 for NULL), or as text ("" for NULL). */
int64_t num(const Cell &c);
std::string text(const Cell &c);

} // namespace pond
