#include "listener.h"

#include "base/log.h"
#include "http/tls.h"

#include <openssl/ssl.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <system_error>
#include <thread>

namespace pond {

constexpr int MAX_CONNECTIONS = 2000;
constexpr int IO_TIMEOUT_SEC = 10;

Listener::~Listener()
{
    if (listener_ != PUDDLE_SOCK_NONE)
        puddle_sock_close(listener_);
    if (have_waker_)
        puddle_waker_close(&stop_);
    SSL_CTX_free(tls_);
}

bool Listener::open(const std::string &address, unsigned port, const std::string &tls_dir, puddle_http_handler handler, std::string &err)
{
    char e[256] = "";
    handler_ = handler;
    tls_ = puddle_tls_context(tls_dir.c_str(), e, sizeof e);
    if (!tls_) {
        err = e;
        return false;
    }
    char fp[65] = "";
    puddle_tls_fingerprint_hex(tls_, fp);
    fingerprint = fp;
    if (puddle_sock_listen(address.c_str(), port, &listener_, e, sizeof e) != 0) {
        err = e;
        return false;
    }
    if (puddle_waker_open(&stop_) != 0) {
        err = "cannot make a stop signal";
        return false;
    }
    have_waker_ = true;
    return true;
}

void Listener::stop()
{
    if (have_waker_)
        puddle_waker_wake(&stop_);
}

static int ssl_read_cb(void *io, void *buf, size_t cap)
{
    SSL *ssl = (SSL *)io;
    errno = 0;
    int n = SSL_read(ssl, buf, (int)cap);
    if (n > 0)
        return n;
    int e = errno;
    if (SSL_get_error(ssl, n) == SSL_ERROR_ZERO_RETURN)
        return 0;
    return (e == EAGAIN || e == EWOULDBLOCK) ? -1 : 0;
}

static int ssl_write_cb(void *io, const void *buf, size_t len)
{
    SSL *ssl = (SSL *)io;
    const char *p = (const char *)buf;
    while (len) {
        int w = SSL_write(ssl, p, (int)len);
        if (w <= 0)
            return -1;
        p += w;
        len -= (size_t)w;
    }
    return 0;
}

void Listener::serve_one(puddle_sock sock, std::string peer)
{
    SSL *ssl = SSL_new(tls_);
    if (ssl && SSL_set_fd(ssl, puddle_sock_fd(sock)) == 1 && SSL_accept(ssl) == 1) {
        puddle_io io = {ssl_read_cb, ssl_write_cb, ssl};
        puddle_serve_info info;
        puddle_http_serve(&handler_, &io, &info);
        if (info.status && strcmp(info.path, "/pond/v1/events") != 0) /* a member's Puddle asks for events all day long */
            puddle_log(PUDDLE_LOG_INFO, "%s %s %s -> %d", peer.c_str(), info.method, info.path, info.status);
        SSL_shutdown(ssl);
        puddle_sock_finish(sock);
    }
    SSL_free(ssl);
    puddle_sock_close(sock);
    std::lock_guard<std::mutex> g(mu_);
    if (--active_ == 0)
        drained_.notify_all();
}

void Listener::run()
{
    for (;;) {
        puddle_sock socks[2] = {listener_, stop_.read_end};
        int which[2];
        int ready = puddle_sock_poll(socks, 2, 60 * 1000, which);
        if (ready == 0)
            continue;
        if (ready < 0) {
            puddle_log(PUDDLE_LOG_ERROR, "poll: %s", strerror(errno));
            break;
        }
        if (which[1] || which[0] == 2)
            break;
        if (!which[0])
            continue;
        char peer[64];
        int out_of_fds;
        puddle_sock sock = puddle_sock_accept(listener_, IO_TIMEOUT_SEC, peer, sizeof peer, &out_of_fds);
        if (sock == PUDDLE_SOCK_NONE) {
            if (out_of_fds)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        {
            std::lock_guard<std::mutex> g(mu_);
            if (active_ >= MAX_CONNECTIONS) {
                puddle_sock_close(sock);
                continue;
            }
            active_++;
        }
        try {
            std::thread(&Listener::serve_one, this, sock, std::string(peer)).detach();
        } catch (const std::system_error &) {
            puddle_sock_close(sock);
            std::lock_guard<std::mutex> g(mu_);
            active_--;
        }
    }
    puddle_sock_close(listener_);
    listener_ = PUDDLE_SOCK_NONE;
}

void Listener::drain()
{
    std::unique_lock<std::mutex> lk(mu_);
    drained_.wait(lk, [&] { return active_ == 0; });
}

} // namespace pond
