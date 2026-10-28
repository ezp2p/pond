#pragma once

/* HTTPS on one port: a thread for each connection, each serving one request with a puddle_http_handler. pondd runs one; so do
 * the tests. */

#include "http/serve.h"
#include "platform/net.h"

#include <condition_variable>
#include <mutex>
#include <string>

typedef struct ssl_ctx_st SSL_CTX;

namespace pond {

class Listener {
public:
    ~Listener();

    /* Listen on address:port with the certificate in `tls_dir` (made self-signed if there is none). false with `err`. */
    bool open(const std::string &address, unsigned port, const std::string &tls_dir, puddle_http_handler handler, std::string &err);

    /* Accept until stop(). */
    void run();

    /* Wait for the connections being served to finish (after run(); stop the Service first, so none is held waiting). */
    void drain();

    /* Safe from a signal handler or another thread. */
    void stop();

    std::string fingerprint; /* the certificate's SHA-256, hex */

private:
    void serve_one(puddle_sock sock, std::string peer);

    SSL_CTX *tls_ = nullptr;
    puddle_http_handler handler_{};
    puddle_sock listener_ = PUDDLE_SOCK_NONE;
    puddle_waker stop_{PUDDLE_SOCK_NONE, PUDDLE_SOCK_NONE};
    bool have_waker_ = false;
    std::mutex mu_;
    std::condition_variable drained_;
    int active_ = 0;
};

} // namespace pond
