#pragma once

/* MariaDB, through Drogon's asynchronous client: statements take `?` placeholders and are awaited (co_await). The schema is
 * in db.cpp (`migrate`). */

#include "config.h"

#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>

#include <string>

namespace pond {

using DbPtr = drogon::orm::DbClientPtr;
using TxPtr = std::shared_ptr<drogon::orm::Transaction>;

/* A client with cfg.connections connections, made as they are needed. It runs threads of its own, so it works the same in
 * pondd and pondctl. */
DbPtr connect_db(const DbConfig &cfg);

/* Make or bring up to date the Pond's tables, waiting for each statement. false with the reason in `err`, which is also how
 * a database that cannot be reached shows. */
bool migrate(const DbPtr &db, std::string &err);

/* Awaits the commit of `tx`, which must be the only reference to it: true if it committed. A transaction in which a
 * statement failed has already been rolled back, and must not be awaited. */
struct Committed : drogon::CallbackAwaiter<bool> {
    explicit Committed(TxPtr &&tx) : tx_(std::move(tx)) {}
    void await_suspend(std::coroutine_handle<> h)
    {
        tx_->setCommitCallback([this, h](bool ok) {
            setValue(ok);
            h.resume();
        });
        tx_.reset(); /* the last reference: Drogon commits as it goes */
    }

private:
    TxPtr tx_;
};

/* A field as a number (0 for NULL), or as text ("" for NULL). */
int64_t num(const drogon::orm::Field &f);
std::string text(const drogon::orm::Field &f);

/* The server's error number behind a database exception (1062 is a duplicate key), or 0. */
int sql_errno(const drogon::orm::DrogonDbException &e);

} // namespace pond
