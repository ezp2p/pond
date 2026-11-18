#pragma once

/* Sealing (the Puddle Protocol, spec/09-sealing.md): what keeps a member's requests and the Pond's answers private whatever
 * carries them, a reverse proxy included, so TLS is transport only and no certificate matters.
 *
 * HPKE (RFC 9180), base mode, the one suite the protocol uses: DHKEM(X25519, HKDF-SHA256), HKDF-SHA256, ChaCha20-Poly1305.
 * Written here from the RFC over OpenSSL 3.0's primitives (its own HPKE needs 3.2; Debian 12 has 3.0), and checked against
 * the RFC's vectors in identity_test. A request's body is sealed to the Pond's key; the answer goes back in records under a
 * key exported from the request's context, so only the member who asked can read it. */

#include <cstdint>
#include <string>
#include <string_view>

namespace pond {

constexpr size_t SEAL_TAG = 16;
constexpr size_t SEAL_RECORD = 65536; /* plaintext in each answer record but the last */
constexpr const char *SEALED_TYPE = "application/puddle-sealed";

struct Hpke {
    unsigned char key[32];
    unsigned char base_nonce[12];
    unsigned char exporter[32];
    uint64_t seq = 0;
};

/* RFC 9180 DeriveKeyPair, and an X25519 key's public half. */
bool x25519_derive(const void *ikm, size_t ikm_len, unsigned char sk[32], unsigned char pk[32]);
bool x25519_public(const unsigned char sk[32], unsigned char pk[32]);

/* A sender's context to `pk_r`: with a fresh one-time key, or (tests) with `sk_e`. `enc` is what the recipient needs. */
bool hpke_sender(const unsigned char pk_r[32], std::string_view info, unsigned char enc[32], Hpke &ctx);
bool hpke_sender_with(const unsigned char sk_e[32], const unsigned char pk_r[32], std::string_view info, unsigned char enc[32], Hpke &ctx);
/* The recipient's context. */
bool hpke_receiver(const unsigned char enc[32], const unsigned char sk_r[32], std::string_view info, Hpke &ctx);

/* Seal or open one message under the context's next sequence number. */
bool hpke_seal(Hpke &ctx, std::string_view aad, std::string_view pt, std::string &out);
bool hpke_open(Hpke &ctx, std::string_view aad, std::string_view ct, std::string &out);

/* HPKE Export */
bool hpke_export(const Hpke &ctx, std::string_view label, unsigned char *out, size_t len);

/* ---- the protocol's pieces ---- */

/* "puddle-seal-v1\n<sender>\n<recipient>\n<nonce>" and "<METHOD>\n<target>" */
std::string seal_info(std::string_view sender, std::string_view recipient, std::string_view nonce);
std::string seal_aad(std::string_view method, std::string_view target);

/* X-Puddle-Seal: v1 kid=<hex SHA-256 of the recipient's key>; enc=<base64>. Strictly, as Puddle writes it. */
bool parse_seal_header(std::string_view header, std::string &kid, unsigned char enc[32]);

/* The answer key a request's context exports ("puddle-answer-v1"), and an answer sealed or opened with it. */
bool answer_key(const Hpke &ctx, unsigned char key[32]);
bool seal_answer(const unsigned char key[32], int status, std::string_view pt, std::string &out);
bool open_answer(const unsigned char key[32], int status, std::string_view ct, std::string &out);

/* The Pond's sealing key, from the seed of its signing key: HKDF-SHA256, info "pond-seal-v1" (spec/09, "Keys"). */
bool pond_seal_key(const unsigned char seed[32], unsigned char sk[32], unsigned char pk[32]);

} // namespace pond
