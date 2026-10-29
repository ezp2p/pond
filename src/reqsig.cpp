#include "reqsig.h"

#include "encoding.h"
#include "identity.h"

#include <cstdlib>

namespace pond {

bool parse_reqsig(std::string_view h, ReqSig &out)
{
    static const std::string_view seps[] = {"v1 id=", "; kid=", "; ts=", "; nonce=", "; sig="};
    if (h.size() > 1000)
        return false;
    std::string_view vals[5];
    size_t p = 0;
    for (int i = 0; i < 5; i++) {
        if (h.compare(p, seps[i].size(), seps[i]) != 0)
            return false;
        p += seps[i].size();
        size_t end = i < 4 ? h.find(seps[i + 1], p) : h.size();
        if (end == std::string_view::npos || end == p || end - p >= 120)
            return false;
        vals[i] = h.substr(p, end - p);
        p = end;
    }
    unsigned char kid[32];
    if (!id_valid(vals[0]) || !unhex(vals[1], kid, 32) || vals[2].size() > 15)
        return false;
    for (char c : vals[2])
        if (c < '0' || c > '9')
            return false;
    if (vals[3].size() < 16 || vals[3].size() >= 48)
        return false;
    for (char c : vals[3])
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '='))
            return false;
    if (!unbase64(vals[4], out.sig, 64))
        return false;
    out.id = vals[0];
    out.kid = vals[1];
    out.ts = strtoll(std::string(vals[2]).c_str(), nullptr, 10);
    out.nonce = vals[3];
    return true;
}

std::string reqsig_canonical(std::string_view method, std::string_view target, std::string_view body, int64_t ts,
                             std::string_view nonce, std::string_view recipient)
{
    std::string s = "puddle-request-v1\n";
    s.append(method).append("\n").append(target).append("\n").append(sha256_hex(body)).append("\n");
    s.append(std::to_string(ts)).append("\n").append(nonce).append("\n").append(recipient);
    return s;
}

constexpr int64_t NONCE_REMEMBER_MS = 2 * SIG_WINDOW_MS;

Replay::Seen Replay::remember(const std::string &sender, const std::string &nonce, int64_t now)
{
    std::lock_guard<std::mutex> g(mu_);
    if (now >= next_purge_ || seen_.size() >= max_) {
        std::erase_if(seen_, [&](const auto &kv) { return kv.second <= now; });
        next_purge_ = now + 60 * 1000;
    }
    auto [it, fresh] = seen_.try_emplace(sender + ":" + nonce, now + NONCE_REMEMBER_MS);
    if (!fresh) {
        if (it->second > now)
            return REPLAYED;
        it->second = now + NONCE_REMEMBER_MS; /* long gone: new again */
        return FRESH;
    }
    if (seen_.size() > max_) {
        seen_.erase(it);
        return FULL;
    }
    return FRESH;
}

Authn authenticate(std::string_view header, std::string_view method, std::string_view target, std::string_view body,
                   std::string_view me, const KeyLookup &lookup, Replay &replay, int64_t now, ReqSig &out)
{
    ReqSig r;
    unsigned char pub[32];
    if (!parse_reqsig(header, r) || !lookup(r.id, r.kid, pub) || key_id(pub) != r.kid)
        return Authn::REJECT;
    if (!verify_signature(pub, reqsig_canonical(method, target, body, r.ts, r.nonce, me), r.sig))
        return Authn::REJECT;
    /* the sender is now known; from here they may be told what is wrong */
    out = r;
    if (now - r.ts > SIG_WINDOW_MS || r.ts - now > SIG_WINDOW_MS)
        return Authn::SKEW;
    switch (replay.remember(r.id, r.nonce, now)) {
    case Replay::FRESH:
        return Authn::OK;
    case Replay::REPLAYED:
        return Authn::REPLAYED;
    case Replay::FULL:
        break;
    }
    return Authn::BUSY;
}

} // namespace pond
