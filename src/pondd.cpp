/* pondd: the Pond service. Serves api/pond.yaml over HTTPS on one port until it is asked to stop (Ctrl-C or SIGTERM).
 * Usage: pondd [-c pond.yaml] */

#include "config.h"
#include "db.h"
#include "key.h"
#include "pond.h"
#include "routes.h"
#include "tls.h"
#include "voice.h"

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

    Service svc(db, key, cfg.data_dir, app().getLoop());
    svc.wait_ms = cfg.events_wait_ms;
    std::string cert = cfg.data_dir + "/cert.pem";
    /* read once: the certificate served is the one loaded at start, so a new one (renewed) takes a restart */
    std::string tls_fp = certificate_fingerprint(cert);
    if (tls_fp.empty()) {
        fprintf(stderr, "pondd: cannot read %s\n", cert.c_str());
        return 1;
    }
    /* voice channels (docs/voice.md): one UDP port, the HTTPS port's number unless said */
    std::unique_ptr<Voice> voice;
    if (cfg.voice.on && Voice::built()) {
        VoiceSettings vs;
        vs.port = cfg.voice.port ? cfg.voice.port : cfg.port;
        vs.bind = cfg.listen == "0.0.0.0" || cfg.listen == "::" ? "" : cfg.listen;
        vs.max_in_channel = cfg.voice.max_in_channel;
        voice = std::make_unique<Voice>(svc, vs, app().getLoop());
        svc.voice_on = true;
        svc.voice_now = [v = voice.get()] { return v->now_json(); };
    }
    Routes routes(svc, tls_fp, voice.get());
    /* every request goes to the routes, whatever its path and method, so that nothing is answered differently from an
     * unknown path (no 405s) */
    app().registerHandlerViaRegex(
        "/.*", [&routes](HttpRequestPtr req) -> Task<HttpResponsePtr> { co_return co_await routes.handle(std::move(req)); },
        {Get, Post, Put, Delete, Patch, Options, Head});
    app().setCustom404Page(Routes::not_found(), false);

    auto stop = [&svc, &voice] {
        LOG_INFO << "stopping";
        if (voice)
            voice->stop(); /* every voice connection closed */
        svc.stop(); /* answers everyone waiting for events, then gives the answers a moment to go out */
        app().getLoop()->runAfter(0.3, [] { app().quit(); });
    };
    app()
        .setLogLevel(trantor::Logger::kInfo)
        .addListener(cfg.listen, (uint16_t)cfg.port, true, cert, cfg.data_dir + "/key.pem")
        .setThreadNum(cfg.threads)
        .enableServerHeader(false)
        .setClientMaxBodySize(2 << 20) /* a chunk of a file and its headers; the routes refuse more than each takes, with a 404 */
        .setClientMaxMemoryBodySize(2 << 20)
        .setIdleConnectionTimeout(120)
        .setTermSignalHandler(stop)
        .setIntSignalHandler(stop);
    app().getLoop()->queueInLoop([&] {
        svc.watch();
        if (voice) {
            voice->start();
            LOG_INFO << "voice channels on UDP port " << (cfg.voice.port ? cfg.voice.port : cfg.port);
        }
        LOG_INFO << "Pond " << key.id << " listening on " << cfg.listen << ":" << cfg.port << " (certificate "
                 << tls_fp << ")";
    });
    app().run();
    return 0;
}
