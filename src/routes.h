#pragma once

/* The Pond API over HTTP (api/pond.yaml): which route a request is, who sent it (reqsig.h), and the Service call it becomes.
 * Anything that fails a check gets the same 404 as a path that does not exist. */

#include "pond.h"
#include "reqsig.h"

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include <mutex>

namespace pond {

class Routes {
public:
    explicit Routes(Service &s) : svc_(s) {}

    /* Every request pondd gets comes here, whatever its method and path. */
    Task<drogon::HttpResponsePtr> handle(drogon::HttpRequestPtr req);

    /* The answer for anything that fails a check: the same as for a path that does not exist. */
    static drogon::HttpResponsePtr not_found();

private:
    Task<Answer> answer(const drogon::HttpRequestPtr &req);
    Service &svc_;
    Replay replay_;
    std::mutex strangers_mu_;
    Bucket strangers_{20, 100}; /* joins, which anyone may send: checking a chain costs signatures */
};

} // namespace pond
