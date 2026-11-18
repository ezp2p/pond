#include "seal.h"

#include "encoding.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/kdf.h>

#include <cstring>

namespace pond {

namespace {

/* the suite's ids: KEM 0x0020, KDF 0x0001, AEAD 0x0003 */
const std::string KEM_SUITE("KEM\x00\x20", 5);
const std::string HPKE_SUITE("HPKE\x00\x20\x00\x01\x00\x03", 10);

std::string hmac(std::string_view key, std::string_view data)
{
    unsigned char out[32];
    unsigned len = 0;
    static const unsigned char zeros[32] = {};
    const void *k = key.empty() ? (const void *)zeros : (const void *)key.data();
    int kl = key.empty() ? 32 : (int)key.size();
    if (!HMAC(EVP_sha256(), k, kl, (const unsigned char *)data.data(), data.size(), out, &len) || len != 32)
        return "";
    return std::string((const char *)out, 32);
}

std::string labeled_extract(const std::string &suite, std::string_view salt, const char *label, std::string_view ikm)
{
    std::string in = "HPKE-v1" + suite + label;
    in.append(ikm);
    std::string prk = hmac(salt, in);
    OPENSSL_cleanse(in.data(), in.size());
    return prk;
}

std::string labeled_expand(const std::string &suite, const std::string &prk, const char *label, std::string_view info, size_t len)
{
    if (prk.size() != 32 || len > 255 * 32)
        return "";
    std::string labeled;
    labeled += (char)(len >> 8);
    labeled += (char)(len & 0xff);
    labeled += "HPKE-v1" + suite + label;
    labeled.append(info);
    std::string out, t;
    for (unsigned char i = 1; out.size() < len; i++) {
        t = hmac(prk, t + labeled + (char)i);
        if (t.empty())
            return "";
        out += t;
    }
    out.resize(len);
    return out;
}

bool dh(const unsigned char sk[32], const unsigned char pk[32], unsigned char out[32])
{
    EVP_PKEY *mine = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr, sk, 32);
    EVP_PKEY *theirs = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, nullptr, pk, 32);
    EVP_PKEY_CTX *c = mine ? EVP_PKEY_CTX_new(mine, nullptr) : nullptr;
    size_t len = 32;
    bool ok = c && theirs && EVP_PKEY_derive_init(c) == 1 && EVP_PKEY_derive_set_peer(c, theirs) == 1 && EVP_PKEY_derive(c, out, &len) == 1 &&
              len == 32;
    unsigned char any = 0;
    for (int i = 0; ok && i < 32; i++)
        any |= out[i];
    EVP_PKEY_CTX_free(c);
    EVP_PKEY_free(mine);
    EVP_PKEY_free(theirs);
    return ok && any; /* all zeros: a key of low order (RFC 9180, 7.1.4) */
}

bool schedule(const unsigned char dh_out[32], const unsigned char enc[32], const unsigned char pk_r[32], std::string_view info, Hpke &ctx)
{
    std::string kem_context((const char *)enc, 32);
    kem_context.append((const char *)pk_r, 32);
    std::string eae = labeled_extract(KEM_SUITE, "", "eae_prk", std::string_view((const char *)dh_out, 32));
    std::string shared = labeled_expand(KEM_SUITE, eae, "shared_secret", kem_context, 32);
    std::string ksc(1, '\0');
    ksc += labeled_extract(HPKE_SUITE, "", "psk_id_hash", "");
    ksc += labeled_extract(HPKE_SUITE, "", "info_hash", info);
    std::string secret = labeled_extract(HPKE_SUITE, shared, "secret", "");
    std::string key = labeled_expand(HPKE_SUITE, secret, "key", ksc, 32), nonce = labeled_expand(HPKE_SUITE, secret, "base_nonce", ksc, 12),
                exp = labeled_expand(HPKE_SUITE, secret, "exp", ksc, 32);
    bool ok = shared.size() == 32 && key.size() == 32 && nonce.size() == 12 && exp.size() == 32;
    if (ok) {
        memcpy(ctx.key, key.data(), 32);
        memcpy(ctx.base_nonce, nonce.data(), 12);
        memcpy(ctx.exporter, exp.data(), 32);
        ctx.seq = 0;
    }
    for (std::string *s : {&eae, &shared, &secret, &key, &nonce, &exp})
        OPENSSL_cleanse(s->data(), s->size());
    return ok;
}

bool aead(bool seal, const unsigned char key[32], const unsigned char nonce[12], std::string_view aad, std::string_view in, std::string &out)
{
    if (!seal && in.size() < SEAL_TAG)
        return false;
    size_t body = seal ? in.size() : in.size() - SEAL_TAG;
    out.assign(body + (seal ? SEAL_TAG : 0), '\0');
    EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
    int n = 0;
    auto *o = (unsigned char *)out.data();
    auto *i = (const unsigned char *)in.data();
    bool ok = c && (seal ? EVP_EncryptInit_ex(c, EVP_chacha20_poly1305(), nullptr, key, nonce)
                         : EVP_DecryptInit_ex(c, EVP_chacha20_poly1305(), nullptr, key, nonce)) == 1;
    if (ok && !aad.empty())
        ok = (seal ? EVP_EncryptUpdate(c, nullptr, &n, (const unsigned char *)aad.data(), (int)aad.size())
                   : EVP_DecryptUpdate(c, nullptr, &n, (const unsigned char *)aad.data(), (int)aad.size())) == 1;
    if (ok && body)
        ok = (seal ? EVP_EncryptUpdate(c, o, &n, i, (int)body) : EVP_DecryptUpdate(c, o, &n, i, (int)body)) == 1;
    if (ok && seal)
        ok = EVP_EncryptFinal_ex(c, o + body, &n) == 1 && EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_AEAD_GET_TAG, SEAL_TAG, o + body) == 1;
    else if (ok)
        ok = EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_AEAD_SET_TAG, SEAL_TAG, (void *)(i + body)) == 1 && EVP_DecryptFinal_ex(c, o + body, &n) == 1;
    EVP_CIPHER_CTX_free(c);
    if (!ok) {
        OPENSSL_cleanse(out.data(), out.size());
        out.clear();
    }
    return ok;
}

void nonce_for(const unsigned char base[12], uint64_t seq, unsigned char out[12])
{
    memcpy(out, base, 12);
    for (int i = 0; i < 8; i++)
        out[11 - i] ^= (unsigned char)(seq >> (8 * i));
}

constexpr const char *ANSWER_LABEL = "puddle-answer-v1";

std::string record_aad(int status, bool last)
{
    return std::string(ANSWER_LABEL) + "\n" + std::to_string(status) + "\n" + (last ? "1" : "0");
}

void record_nonce(uint64_t index, unsigned char out[12])
{
    memset(out, 0, 12);
    for (int i = 0; i < 8; i++)
        out[11 - i] = (unsigned char)(index >> (8 * i));
}

} // namespace

bool x25519_public(const unsigned char sk[32], unsigned char pk[32])
{
    EVP_PKEY *k = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr, sk, 32);
    size_t len = 32;
    bool ok = k && EVP_PKEY_get_raw_public_key(k, pk, &len) == 1 && len == 32;
    EVP_PKEY_free(k);
    return ok;
}

bool x25519_derive(const void *ikm, size_t ikm_len, unsigned char sk[32], unsigned char pk[32])
{
    std::string prk = labeled_extract(KEM_SUITE, "", "dkp_prk", std::string_view((const char *)ikm, ikm_len));
    std::string s = labeled_expand(KEM_SUITE, prk, "sk", "", 32);
    bool ok = s.size() == 32;
    if (ok)
        memcpy(sk, s.data(), 32);
    OPENSSL_cleanse(prk.data(), prk.size());
    OPENSSL_cleanse(s.data(), s.size());
    return ok && x25519_public(sk, pk);
}

bool hpke_sender_with(const unsigned char sk_e[32], const unsigned char pk_r[32], std::string_view info, unsigned char enc[32], Hpke &ctx)
{
    unsigned char d[32];
    bool ok = x25519_public(sk_e, enc) && dh(sk_e, pk_r, d) && schedule(d, enc, pk_r, info, ctx);
    OPENSSL_cleanse(d, sizeof d);
    return ok;
}

bool hpke_sender(const unsigned char pk_r[32], std::string_view info, unsigned char enc[32], Hpke &ctx)
{
    unsigned char ikm[32], sk[32], pk[32];
    bool ok = random_bytes(ikm, sizeof ikm) && x25519_derive(ikm, sizeof ikm, sk, pk) && hpke_sender_with(sk, pk_r, info, enc, ctx);
    OPENSSL_cleanse(ikm, sizeof ikm);
    OPENSSL_cleanse(sk, sizeof sk);
    return ok;
}

bool hpke_receiver(const unsigned char enc[32], const unsigned char sk_r[32], std::string_view info, Hpke &ctx)
{
    unsigned char pk_r[32], d[32];
    bool ok = x25519_public(sk_r, pk_r) && dh(sk_r, enc, d) && schedule(d, enc, pk_r, info, ctx);
    OPENSSL_cleanse(d, sizeof d);
    return ok;
}

bool hpke_seal(Hpke &ctx, std::string_view aad, std::string_view pt, std::string &out)
{
    unsigned char nonce[12];
    nonce_for(ctx.base_nonce, ctx.seq, nonce);
    if (ctx.seq == UINT64_MAX || !aead(true, ctx.key, nonce, aad, pt, out))
        return false;
    ctx.seq++;
    return true;
}

bool hpke_open(Hpke &ctx, std::string_view aad, std::string_view ct, std::string &out)
{
    unsigned char nonce[12];
    nonce_for(ctx.base_nonce, ctx.seq, nonce);
    if (ctx.seq == UINT64_MAX || !aead(false, ctx.key, nonce, aad, ct, out))
        return false;
    ctx.seq++;
    return true;
}

bool hpke_export(const Hpke &ctx, std::string_view label, unsigned char *out, size_t len)
{
    std::string v = labeled_expand(HPKE_SUITE, std::string((const char *)ctx.exporter, 32), "sec", label, len);
    if (v.size() != len)
        return false;
    memcpy(out, v.data(), len);
    OPENSSL_cleanse(v.data(), v.size());
    return true;
}

std::string seal_info(std::string_view sender, std::string_view recipient, std::string_view nonce)
{
    return "puddle-seal-v1\n" + std::string(sender) + "\n" + std::string(recipient) + "\n" + std::string(nonce);
}

std::string seal_aad(std::string_view method, std::string_view target)
{
    return std::string(method) + "\n" + std::string(target);
}

bool parse_seal_header(std::string_view h, std::string &kid, unsigned char enc[32])
{
    static const std::string_view A = "v1 kid=", B = "; enc=";
    if (h.substr(0, A.size()) != A)
        return false;
    size_t b = h.find(B, A.size());
    if (b == std::string_view::npos)
        return false;
    std::string_view k = h.substr(A.size(), b - A.size()), e = h.substr(b + B.size());
    unsigned char raw[32];
    if (k.size() != 64 || !unhex(k, raw, 32) || !unbase64(e, enc, 32))
        return false;
    kid = std::string(k);
    return true;
}

bool answer_key(const Hpke &ctx, unsigned char key[32])
{
    return hpke_export(ctx, ANSWER_LABEL, key, 32);
}

bool seal_answer(const unsigned char key[32], int status, std::string_view pt, std::string &out)
{
    size_t n = pt.empty() ? 1 : (pt.size() + SEAL_RECORD - 1) / SEAL_RECORD;
    out.clear();
    out.reserve(pt.size() + n * SEAL_TAG);
    for (size_t i = 0; i < n; i++) {
        unsigned char nonce[12];
        record_nonce(i, nonce);
        std::string rec;
        if (!aead(true, key, nonce, record_aad(status, i + 1 == n), pt.substr(i * SEAL_RECORD, SEAL_RECORD), rec))
            return false;
        out += rec;
    }
    return true;
}

bool open_answer(const unsigned char key[32], int status, std::string_view ct, std::string &out)
{
    const size_t full = SEAL_RECORD + SEAL_TAG;
    out.clear();
    for (uint64_t i = 0;; i++) {
        size_t at = i * full;
        if (at >= ct.size() && i > 0)
            return false; /* the last record never came */
        bool last = ct.size() - at <= full;
        unsigned char nonce[12];
        record_nonce(i, nonce);
        std::string rec;
        if (!aead(false, key, nonce, record_aad(status, last), ct.substr(at, full), rec))
            return false;
        out += rec;
        if (last)
            return true;
    }
}

bool pond_seal_key(const unsigned char seed[32], unsigned char sk[32], unsigned char pk[32])
{
    EVP_KDF *kdf = EVP_KDF_fetch(nullptr, "HKDF", nullptr);
    EVP_KDF_CTX *c = kdf ? EVP_KDF_CTX_new(kdf) : nullptr;
    OSSL_PARAM params[4];
    static const char info[] = "pond-seal-v1";
    params[0] = OSSL_PARAM_construct_utf8_string("digest", (char *)"SHA256", 0);
    params[1] = OSSL_PARAM_construct_octet_string("key", (void *)seed, 32);
    params[2] = OSSL_PARAM_construct_octet_string("info", (void *)info, sizeof info - 1);
    params[3] = OSSL_PARAM_construct_end();
    bool ok = c && EVP_KDF_derive(c, sk, 32, params) == 1 && x25519_public(sk, pk);
    EVP_KDF_CTX_free(c);
    EVP_KDF_free(kdf);
    return ok;
}

} // namespace pond
