#pragma once

/* The Pond's HTTP routes (api/pond.yaml): who may call each one, and what it calls in the Service. Every request is signed by a
 * member's Puddle; anything that fails a check is answered exactly like a path that does not exist. */

#include "pond.h"

#include "http/serve.h"

typedef struct puddle_replay puddle_replay;
typedef struct puddle_bucket puddle_bucket;

namespace pond {

class Routes {
public:
    explicit Routes(Service &s);
    ~Routes();

    /* A handler for puddle_http_serve(), answering for this Pond. */
    puddle_http_handler handler();

    /* One request, already read: the status and body. For tests, which call it without a socket. */
    Answer handle(const puddle_request &req, const unsigned char *body, size_t body_len);

    int64_t (*clock)() = nullptr; /* ms; NULL = the wall clock. A seam for tests. */

private:
    Service &svc_;
    puddle_replay *replay_;
    puddle_bucket *strangers_; /* work for requests from non-members (checking a chain costs signatures) */
};

} // namespace pond
