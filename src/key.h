#pragma once

/* The Pond's key (docs/design.md, "Who a Pond is"): an Ed25519 key pair in <data_dir>/pond.key, mode 0600. Its id is made like a
 * Puddle's user id, base32 of the SHA-256 of the public key, and is what members sign their requests for. */

#include <memory>
#include <string>
#include <string_view>

typedef struct evp_pkey_st EVP_PKEY;

namespace pond {

struct PondKey {
    unsigned char pub[32];
    std::string id; /* 52 characters */
    std::shared_ptr<EVP_PKEY> secret; /* the private key, held for signing receipts (load_key fills it; create_key does not) */

    /* Ed25519 over `data`. false if this key has no private half, or signing failed. */
    bool sign(std::string_view data, unsigned char sig[64]) const;
};

/* Make a key in `data_dir` (which is made too). Refuses if one is already there: a Pond with a new key is a different Pond. */
bool create_key(const std::string &data_dir, PondKey &out, std::string &err);

bool load_key(const std::string &data_dir, PondKey &out, std::string &err);

} // namespace pond
