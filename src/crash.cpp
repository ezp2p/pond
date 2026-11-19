#include "crash.h"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <execinfo.h>
#include <unistd.h>

namespace pond {

namespace {

/* A fixed table, written with plain stores and read by the signal handler without locks or allocation. A request that finds
 * every slot taken is simply not listed. */
constexpr int SLOTS = 128;
struct Slot {
    std::atomic<bool> used{false};
    char text[192];
};
Slot slots[SLOTS];

void say(const char *s)
{
    size_t n = strlen(s);
    while (n > 0) {
        ssize_t w = write(STDERR_FILENO, s, n);
        if (w <= 0)
            return;
        s += w;
        n -= (size_t)w;
    }
}

const char *signal_name(int sig)
{
    switch (sig) {
    case SIGSEGV: return "SIGSEGV (a bad pointer)";
    case SIGABRT: return "SIGABRT (aborted, e.g. by glibc finding the heap damaged)";
    case SIGBUS: return "SIGBUS";
    case SIGILL: return "SIGILL";
    case SIGFPE: return "SIGFPE";
    default: return "a signal";
    }
}

void on_crash(int sig)
{
    /* only what is safe in a signal handler: write(2), and backtrace, loaded beforehand */
    say("pondd crashed: ");
    say(signal_name(sig));
    say("\n");
    bool any = false;
    for (Slot &s : slots) {
        if (!s.used.load(std::memory_order_acquire))
            continue;
        say(any ? "  " : "pondd crashed while answering:\n  ");
        say(s.text);
        say("\n");
        any = true;
    }
    if (!any)
        say("pondd crashed while answering no request\n");
    say("where (pondd crash backtrace):\n");
    void *frames[48];
    int n = backtrace(frames, 48);
    backtrace_symbols_fd(frames, n, STDERR_FILENO);
    /* then die of it as before, so the exit code and a core dump (if wanted) are the same */
    signal(sig, SIG_DFL);
    raise(sig);
    /* still here: pondd is PID 1 in its container, and the kernel ignores a signal PID 1 raises at itself */
    _exit(128 + sig);
}

} // namespace

void crash_report_install()
{
    void *warm[1];
    backtrace(warm, 1); /* loads libgcc now: the first call may allocate, which a crashed heap can't */
    for (int sig : {SIGSEGV, SIGABRT, SIGBUS, SIGILL, SIGFPE}) {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = on_crash;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_RESETHAND | SA_NODEFER;
        sigaction(sig, &sa, nullptr);
    }
}

InFlight::InFlight(const std::string &method, const std::string &path, const std::string &peer) : slot_(-1)
{
    for (int i = 0; i < SLOTS; i++) {
        bool expected = false;
        if (slots[i].used.load(std::memory_order_relaxed) ||
            !slots[i].used.compare_exchange_strong(expected, true, std::memory_order_acquire))
            continue;
        /* written while the slot reads as taken but before it is published as filled: a crash at this instant prints a
         * half-written line, which is harmless (the text always ends in a NUL) */
        slots[i].text[0] = '\0';
        snprintf(slots[i].text, sizeof slots[i].text, "%s %s from %s", method.c_str(), path.c_str(), peer.c_str());
        std::atomic_thread_fence(std::memory_order_release);
        slot_ = i;
        return;
    }
}

InFlight::~InFlight()
{
    if (slot_ >= 0)
        slots[slot_].used.store(false, std::memory_order_release);
}

} // namespace pond
