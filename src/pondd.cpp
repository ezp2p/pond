/* pondd: the Pond service. Serves api/pond.yaml over HTTPS on one port until it is asked to stop (Ctrl-C or SIGTERM).
 * Usage: pondd [-c pond.yaml] */

#include "config.h"
#include "db.h"
#include "key.h"
#include "pond.h"
#include "routes.h"
#include "tls.h"

#include <drogon/drogon.h>

#include <cstdio>
#include <cstring>

using namespace pond;
using namespace drogon;

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
    /* each line as it is logged, also when stdout is a file or a pipe (a service manager's journal) */
    trantor::Logger::setOutputFunction([](const char *msg, uint64_t len) { fwrite(msg, 1, len, stdout); }, [] { fflush(stdout); });
    setvbuf(stdout, nullptr, _IOLBF, 0);
    Config cfg;
    std::string err;
    PondKey key;
    if (!load_config(path, cfg, err) || !load_key(cfg.data_dir, key, err) || !ensure_certificate(cfg.data_dir, err)) {
        fprintf(stderr, "pondd: %s\n", err.c_str());
        return 1;
    }
    DbPtr db = connect_db(cfg.db);
    if (!migrate(db, err)) {
        fprintf(stderr, "pondd: %s\n", err.c_str());
        return 1;
    }

    Service svc(db, key, app().getLoop());
    svc.wait_ms = cfg.events_wait_ms;
    Routes routes(svc);
    /* every request goes to the routes, whatever its path and method, so that nothing is answered differently from an
     * unknown path (no 405s) */
    app().registerHandlerViaRegex(
        "/.*", [&routes](HttpRequestPtr req) -> Task<HttpResponsePtr> { co_return co_await routes.handle(std::move(req)); },
        {Get, Post, Put, Delete, Patch, Options, Head});
    app().setCustom404Page(Routes::not_found(), false);

    auto stop = [&svc] {
        LOG_INFO << "stopping";
        svc.stop(); /* answers everyone waiting for events, then gives the answers a moment to go out */
        app().getLoop()->runAfter(0.3, [] { app().quit(); });
    };
    std::string cert = cfg.data_dir + "/cert.pem";
    app()
        .setLogLevel(trantor::Logger::kInfo)
        .addListener(cfg.listen, (uint16_t)cfg.port, true, cert, cfg.data_dir + "/key.pem")
        .setThreadNum(cfg.threads)
        .enableServerHeader(false)
        .setClientMaxBodySize(1 << 20) /* the routes refuse more than 32 KiB, with the same 404 as anything else */
        .setClientMaxMemoryBodySize(1 << 20)
        .setIdleConnectionTimeout(120)
        .setTermSignalHandler(stop)
        .setIntSignalHandler(stop);
    app().getLoop()->queueInLoop([&] {
        svc.watch();
        LOG_INFO << "Pond " << key.id << " listening on " << cfg.listen << ":" << cfg.port << " (certificate "
                 << certificate_fingerprint(cert) << ")";
    });
    app().run();
    return 0;
}
