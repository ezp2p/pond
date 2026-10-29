#include "identity.h"

#include "encoding.h"
#include "json.h"

#include <openssl/evp.h>

#include <cstring>

namespace pond {

constexpr size_t CHAIN_MAX_BYTES = 4u << 20;
constexpr size_t CHAIN_MAX_RECORDS = 10000;

bool id_valid(std::string_view s)
{
    if (s.size() != ID_LEN)
        return false;
    for (char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= '2' && c <= '7')))
            return false;
    return true;
}

std::string key_id(const unsigned char pub[32])
{
    unsigned char h[32];
    sha256(pub, 32, h);
    return hex(h, 32);
}

bool verify_signature(const unsigned char pub[32], std::string_view data, const unsigned char sig[64])
{
    EVP_PKEY *k = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, pub, 32);
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    bool ok = k && ctx && EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, k) == 1 &&
              EVP_DigestVerify(ctx, sig, 64, (const unsigned char *)data.data(), data.size()) == 1;
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(k);
    return ok;
}

/* A string member of a record that must decode to exactly `n` bytes: base64 for keys and signatures. */
static bool b64_member(const Json::Value &obj, const char *key, unsigned char *out, size_t n)
{
    std::string s;
    return get_string(obj, key, s) && unbase64(s, out, n);
}

static bool hex_member(const Json::Value &obj, const char *key, std::string &out)
{
    unsigned char raw[32];
    return get_string(obj, key, out) && unhex(out, raw, 32);
}

static bool fail(std::string &why, size_t line, const char *what)
{
    why = "chain record " + std::to_string(line) + ": " + what;
    return false;
}

bool verify_chain(std::string_view text, ChainHead &out, std::string &why)
{
    if (text.empty() || text.size() > CHAIN_MAX_BYTES)
        return fail(why, 0, "empty or too large");
    ChainHead h;
    std::string next, head_hash; /* the commitment to the next roll key, and the hash of the last record */
    size_t count = 0, pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        std::string_view line = text.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
        pos = nl == std::string_view::npos ? text.size() : nl + 1;
        if (line.empty())
            continue;
        if (++count > CHAIN_MAX_RECORDS)
            return fail(why, count, "too many records");

        Json::Value all, rec;
        std::string_view raw; /* the record's exact bytes: what the signatures cover */
        unsigned char sig[64];
        if (!parse_object(line, all) || !raw_member(line, "record", raw) || !all["record"].isObject() ||
            !b64_member(all, "sig", sig, 64))
            return fail(why, count, "malformed");
        rec = all["record"];
        std::string type;
        unsigned char sp[32], ep[32];
        std::string commit;
        if (!get_string(rec, "type", type) || !rec["seq"].isIntegral() || !b64_member(rec, "signing", sp, 32) ||
            !b64_member(rec, "encryption", ep, 32) || !hex_member(rec, "next", commit))
            return fail(why, count, "missing type, seq, keys or commitment");
        int64_t seq = rec["seq"].asInt64();

        if (count == 1) {
            if (type != "identity.inception" || seq != 0)
                return fail(why, count, "the chain must start with an inception record");
            unsigned char both[64], digest[32];
            memcpy(both, sp, 32);
            unhex(commit, both + 32, 32);
            sha256(both, 64, digest);
            h.id = base32(digest, 32);
            if (!verify_signature(sp, raw, sig))
                return fail(why, count, "bad signature");
        } else {
            std::string prev;
            unsigned char roll[32], sig_roll[64], sig_prev[64];
            if (type != "identity.rotate" || seq < 0 || (uint64_t)seq != h.seq + 1)
                return fail(why, count, "out of order");
            if (!hex_member(rec, "prev", prev) || prev != head_hash)
                return fail(why, count, "does not follow the previous record");
            if (!b64_member(rec, "roll", roll, 32) || !b64_member(all, "sig_roll", sig_roll, 64) || !b64_member(all, "sig_prev", sig_prev, 64))
                return fail(why, count, "malformed");
            unsigned char rh[32];
            sha256(roll, 32, rh);
            if (hex(rh, 32) != next)
                return fail(why, count, "the roll key is not the one the previous record committed to");
            if (!verify_signature(roll, raw, sig_roll))
                return fail(why, count, "bad signature from the roll key");
            if (!verify_signature(sp, raw, sig))
                return fail(why, count, "bad signature from the new key");
            if (!verify_signature(h.sign_pub, raw, sig_prev))
                return fail(why, count, "bad signature from the previous key");
        }
        h.seq = (uint64_t)seq;
        memcpy(h.sign_pub, sp, 32);
        next = commit;
        head_hash = sha256_hex(raw);
    }
    if (count == 0)
        return fail(why, 0, "empty");
    out = h;
    return true;
}

} // namespace pond
