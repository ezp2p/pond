#pragma once

/* Bytes as text, hashes and randomness, in the exact forms the Pond API uses (api/pond.yaml): ids and invite tokens are
 * base32, keys and signatures base64, key ids and commitments hex. */

#include <cstddef>
#include <string>
#include <string_view>

namespace pond {

/* RFC 4648 base32, lowercase, no padding: 32 bytes give 52 characters (an id), 16 give 26 (an invite token). */
std::string base32(const unsigned char *in, size_t len);

/* Standard base64 with padding. */
std::string base64(const unsigned char *in, size_t len);
/* Strict: padding required, no whitespace, no stray bits. Exactly `want` bytes into `out`, or false. */
bool unbase64(std::string_view in, unsigned char *out, size_t want);

/* Lowercase hex. */
std::string hex(const unsigned char *in, size_t len);
/* Exactly `want` bytes from 2 * `want` lowercase hex characters, or false. */
bool unhex(std::string_view in, unsigned char *out, size_t want);

void sha256(const void *data, size_t len, unsigned char out[32]);
std::string sha256_hex(std::string_view data);

/* `len` bytes from the system's secure random source; false if it fails. */
bool random_bytes(unsigned char *out, size_t len);

} // namespace pond
