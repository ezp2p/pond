#pragma once

/* A member's identity, as a Puddle proves it: its key chain (Puddle's docs/identity.md, "Key chains"), checked here
 * independently of Puddle's own code. A chain is one JSON record per line; the first is signed by the first signing key, and
 * each later one by the roll key the record before committed to, the new signing key and the previous signing key. The id is
 * base32(SHA-256(first signing key || SHA-256(first roll key))), so a chain proves which id it belongs to with no registry. */

#include <cstdint>
#include <string>
#include <string_view>

namespace pond {

constexpr size_t ID_LEN = 52;

/* 52 characters of a-z and 2-7. */
bool id_valid(std::string_view s);

/* The hex SHA-256 of a signing public key: what a request names as `kid`. */
std::string key_id(const unsigned char pub[32]);

/* Ed25519. */
bool verify_signature(const unsigned char pub[32], std::string_view data, const unsigned char sig[64]);

/* What a valid chain says now. */
struct ChainHead {
    std::string id;
    uint64_t seq = 0;
    unsigned char sign_pub[32] = {};
};

/* Check a whole chain. false with the reason in `why`. */
bool verify_chain(std::string_view text, ChainHead &out, std::string &why);

} // namespace pond
