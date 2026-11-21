#include "reqsig.h"

#include "encoding.h"
#include "identity.h"

#include <puddle_proto.h>

#include <cstdlib>
#include <cstring>

namespace pond {

bool parse_reqsig(std::string_view h, ReqSig &out)
{
    pp_reqsig r;
    if (pp_reqsig_parse(std::string(h).c_str(), &r) != 0)
        return false;
    out.id = r.id;
    out.kid = r.kid;
    out.ts = r.ts;
    out.nonce = r.nonce;
    memcpy(out.sig, r.sig, 64);
    return true;
}

std::string reqsig_canonical(std::string_view method, std::string_view target, std::string_view body, int64_t ts,
                             std::string_view nonce, std::string_view recipient)
{
    char buf[2400];
    int n = pp_reqsig_canonical(buf, sizeof buf, std::string(method).c_str(), std::string(target).c_str(), body.data(), body.size(), ts,
                                std::string(nonce).c_str(), std::string(recipient).c_str());
    return n < 0 ? std::string() : std::string(buf, (size_t)n);
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

std::string receipt_canonical(std::string_view id, std::string_view nonce, int status, std::string_view body)
{
    std::string s = "puddle-receipt-v2\n";
    s.append(id).append("\n").append(nonce).append("\n").append(std::to_string(status)).append("\n");
    s.append(sha256_hex(body));
    return s;
}

static int key_sign(void *user, const void *data, size_t len, unsigned char sig[64])
{
    return ((const PondKey *)user)->sign(std::string_view((const char *)data, len), sig) ? 0 : -1;
}

std::string receipt_header(const PondKey &key, std::string_view nonce, int status, std::string_view body)
{
    pp_signer s{};
    s.id = key.id.c_str();
    memcpy(s.pub, key.pub, 32);
    s.sign = key_sign;
    s.user = (void *)&key;
    char out[PP_RECEIPT_MAX];
    if (pp_receipt_sign(&s, std::string(nonce).c_str(), status, sha256_hex(body).c_str(), out, sizeof out) != 0)
        return "";
    return out;
}

Receipt check_receipt(std::string_view h, std::string_view id, const unsigned char pub[32], std::string_view nonce, int status,
                      std::string_view body)
{
    switch (pp_receipt_verify(std::string(h).c_str(), std::string(id).c_str(), pub, std::string(nonce).c_str(), status,
                              sha256_hex(body).c_str())) {
    case PP_RECEIPT_OK:
        return Receipt::OK;
    case PP_RECEIPT_MISSING:
        return Receipt::MISSING;
    case PP_RECEIPT_WRONG_KEY:
        return Receipt::WRONG_KEY;
    default:
        return Receipt::BAD;
    }
}

} // namespace pond
