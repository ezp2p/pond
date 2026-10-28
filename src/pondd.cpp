/* pondd: the Pond service. Serves api/pond.yaml over HTTPS on one port until it is asked to stop (Ctrl-C or SIGTERM).
 * Usage: pondd [-c pond.yaml] */

#include "config.h"
#include "db.h"
#include "key.h"
#include "listener.h"
#include "pond.h"
#include "routes.h"

#include "base/log.h"
#include "platform/process.h"

#include <cstdio>
#include <cstring>
#include <thread>

using namespace pond;

static Listener *g_listener;

static void on_stop()
{
    g_listener->stop(); /* only wakes it: safe in a signal handler */
}

int main(int argc, char **argv)
{
    std::string path = "pond.yaml";
    for (int i = 1; i < argc; i++) {
        if ((!strcmp(argv[i], "-c") || !strcmp(argv[i], "--config")) && i + 1 < argc) {
            path = argv[++i];
        } else {
            fprintf(stderr, "usage: pondd [-c pond.yaml]\n");
            return 2;
        }
    }
    Config cfg;
    std::string err;
    PondKey key;
    if (!load_config(path, cfg, err) || !load_key(cfg.data_dir, key, err)) {
        fprintf(stderr, "pondd: %s\n", err.c_str());
        return 1;
    }
    Pool pool(cfg.db, 64);
    {
        auto db = pool.get();
        if (!db || !migrate(*db, err)) {
            fprintf(stderr, "pondd: %s\n", db ? err.c_str() : "cannot reach the database");
            return 1;
        }
    }
    Service svc(pool, key);
    Routes routes(svc);
    Listener listener;
    if (!listener.open(cfg.listen, cfg.port, cfg.data_dir, routes.handler(), err)) {
        fprintf(stderr, "pondd: %s\n", err.c_str());
        return 1;
    }
    g_listener = &listener;
    puddle_ignore_broken_pipe();
    puddle_on_stop_signal(on_stop);

    std::thread watcher([&] { svc.watch(); });
    puddle_log(PUDDLE_LOG_INFO, "Pond %s listening on %s:%u (certificate %s)", key.id.c_str(), cfg.listen.c_str(), cfg.port,
               listener.fingerprint.c_str());
    listener.run();
    puddle_log(PUDDLE_LOG_INFO, "stopping");
    svc.stop(); /* answers everyone waiting for events */
    watcher.join();
    listener.drain();
    return 0;
}
