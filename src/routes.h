#pragma once

/* The Pond API over HTTP (api/pond.yaml): which route a request is, who sent it (reqsig.h), and the Service call it becomes.
 * Anything that fails a check gets the same 404 as a path that does not exist. Every other answer to a member carries the
 * Pond's receipt (reqsig.h), signed over the Pond's own certificate; so does a join that worked. */

#include "pond.h"
#include "reqsig.h"

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include <mutex>

namespace pond {

class Routes {
public:
    /* `tls_fp`: the hex SHA-256 of the certificate pondd serves, which receipts name */
    Routes(Service &s, std::string tls_fp) : svc_(s), tls_fp_(std::move(tls_fp)) {}

    /* Every request pondd gets comes here, whatever its method and path. */
    Task<drogon::HttpResponsePtr> handle(drogon::HttpRequestPtr req);

    /* The answer for anything that fails a check: the same as for a path that does not exist. */
    static drogon::HttpResponsePtr not_found();

private:
    /* Who an answer may be signed for: the nonce of a request that authenticated, if any. A join's answer is signed only
     * when it worked, so a receipt never tells someone without a good invite which Pond lives here. */
    struct Signing {
        std::string nonce;
        bool only_ok = false;
    };
    Task<Answer> answer(const drogon::HttpRequestPtr &req, Signing &signing);
    Service &svc_;
    std::string tls_fp_;
    Replay replay_;
    std::mutex strangers_mu_;
    Bucket strangers_{20, 100}; /* joins, which anyone may send: checking a chain costs signatures */
};

} // namespace pond
