#pragma once

/* Request signatures, as a member's Puddle makes them (Puddle's docs/streams.md, "Request signatures"):
 *
 *   X-Puddle-Signature: v1 id=<sender id>; kid=<key id>; ts=<ms>; nonce=<base64>; sig=<base64>
 *
 * `sig` is an Ed25519 signature over these lines, joined by "\n" with none at the end:
 *
 *   puddle-request-v1, METHOD, the request target (path and query, as sent), the hex SHA-256 of the body, ts, nonce, and the
 *   recipient's id (the Pond's id, or "*" for a join)
 *
 * Checked here independently of Puddle's own code. */

#include "key.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace pond {

constexpr int64_t SIG_WINDOW_MS = 5 * 60 * 1000;

struct ReqSig {
    std::string id, kid, nonce;
    int64_t ts = 0;
    unsigned char sig[64] = {};
};

/* Strictly: the exact field order and formats, nothing extra. */
bool parse_reqsig(std::string_view header, ReqSig &out);

/* The exact bytes that are signed. */
std::string reqsig_canonical(std::string_view method, std::string_view target, std::string_view body, int64_t ts,
                             std::string_view nonce, std::string_view recipient);

/* Nonces seen in the last ten minutes, so a captured request cannot be sent again. */
class Replay {
public:
    explicit Replay(size_t max = 1 << 18) : max_(max) {}
    enum Seen { FRESH, REPLAYED, FULL };
    Seen remember(const std::string &sender, const std::string &nonce, int64_t now_ms);

private:
    size_t max_;
    std::mutex mu_;
    std::unordered_map<std::string, int64_t> seen_; /* "<id>:<nonce>" -> when it may be forgotten */
    int64_t next_purge_ = 0;
};

enum class Authn {
    OK,
    REJECT,   /* not authenticated: answer as if the path did not exist */
    SKEW,     /* authenticated, but ts is more than five minutes off */
    REPLAYED, /* authenticated, but this exact request was already seen */
    BUSY,     /* authenticated, but there is no room to remember its nonce */
};

/* The sender's current signing key, if `kid` names it: false for anyone unknown, or a key they no longer use. */
using KeyLookup = std::function<bool(const std::string &id, const std::string &kid, unsigned char pub[32])>;

/* In this order, stopping at the first failure: parse; the sender's key; the signature (which covers `me`, so a request
 * meant for someone else fails here); the clock; the nonce. Everything up to the signature is REJECT. `out` is filled once
 * the signature has passed. */
Authn authenticate(std::string_view header, std::string_view method, std::string_view target, std::string_view body,
                   std::string_view me, const KeyLookup &lookup, Replay &replay, int64_t now_ms, ReqSig &out);

/* Answer receipts (the Puddle Protocol, spec/02-signatures.md and spec/09-sealing.md): what the Pond adds to every answer to a
 * member,
 *
 *   X-Puddle-Receipt: v2 id=<pond id>; kid=<hex SHA-256 of the Pond's key>; sig=<base64>
 *
 * `sig` is the Pond's key over these lines, joined by "\n" with none at the end: puddle-receipt-v2, the Pond's id, the request's
 * nonce, the HTTP status, and the hex SHA-256 of the answer's body exactly as sent (sealed). No certificate: TLS is transport only. */
std::string receipt_canonical(std::string_view id, std::string_view nonce, int status, std::string_view body);

/* The header's value, signed by `key`; "" if it could not sign. */
std::string receipt_header(const PondKey &key, std::string_view nonce, int status, std::string_view body);

/* A receipt checked as a caller would, knowing the responder's id and key and the nonce it sent. Pond only makes receipts; this
 * is for its tests and the protocol's vectors, and says what Puddle's checker says. */
enum class Receipt { OK, MISSING, WRONG_KEY, BAD };
Receipt check_receipt(std::string_view header, std::string_view id, const unsigned char pub[32], std::string_view nonce, int status,
                      std::string_view body);

} // namespace pond
