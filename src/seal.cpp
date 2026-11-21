#include "seal.h"

#include <puddle_proto.h>

#include <cstring>

/* Sealing is libpuddle-proto's (third_party/protocol/lib, checked there against RFC 9180's vectors); these are the Pond's
 * std::string faces on it. A Hpke here is the library's pp_hpke, field for field. */

namespace pond {

static_assert(sizeof(Hpke) == sizeof(pp_hpke), "pond::Hpke must match pp_hpke");

static pp_hpke *pp(Hpke &c)
{
    return reinterpret_cast<pp_hpke *>(&c);
}

static const pp_hpke *pp(const Hpke &c)
{
    return reinterpret_cast<const pp_hpke *>(&c);
}

bool x25519_derive(const void *ikm, size_t ikm_len, unsigned char sk[32], unsigned char pk[32])
{
    return pp_x25519_derive(ikm, ikm_len, sk, pk) == 0;
}

bool x25519_public(const unsigned char sk[32], unsigned char pk[32])
{
    return pp_x25519_public(sk, pk) == 0;
}

bool hpke_sender(const unsigned char pk_r[32], std::string_view info, unsigned char enc[32], Hpke &ctx)
{
    return pp_hpke_sender(pk_r, info.data(), info.size(), enc, pp(ctx)) == 0;
}

bool hpke_sender_with(const unsigned char sk_e[32], const unsigned char pk_r[32], std::string_view info, unsigned char enc[32], Hpke &ctx)
{
    return pp_hpke_sender_with(sk_e, pk_r, info.data(), info.size(), enc, pp(ctx)) == 0;
}

bool hpke_receiver(const unsigned char enc[32], const unsigned char sk_r[32], std::string_view info, Hpke &ctx)
{
    return pp_hpke_receiver(enc, sk_r, info.data(), info.size(), pp(ctx)) == 0;
}

bool hpke_seal(Hpke &ctx, std::string_view aad, std::string_view pt, std::string &out)
{
    out.assign(pt.size() + SEAL_TAG, '\0');
    return pp_hpke_seal(pp(ctx), aad.data(), aad.size(), pt.data(), pt.size(), (unsigned char *)out.data()) == 0;
}

bool hpke_open(Hpke &ctx, std::string_view aad, std::string_view ct, std::string &out)
{
    if (ct.size() < SEAL_TAG)
        return false;
    out.assign(ct.size() - SEAL_TAG, '\0');
    return pp_hpke_open(pp(ctx), aad.data(), aad.size(), (const unsigned char *)ct.data(), ct.size(), (unsigned char *)out.data()) == 0;
}

bool hpke_export(const Hpke &ctx, std::string_view label, unsigned char *out, size_t len)
{
    return pp_hpke_export(pp(ctx), label.data(), label.size(), out, len) == 0;
}

std::string seal_info(std::string_view sender, std::string_view recipient, std::string_view nonce)
{
    char buf[300];
    size_t n = pp_seal_info(std::string(sender).c_str(), std::string(recipient).c_str(), std::string(nonce).c_str(), buf, sizeof buf);
    return std::string(buf, n);
}

std::string seal_aad(std::string_view method, std::string_view target)
{
    std::string m(method), t(target);
    std::string buf(m.size() + t.size() + 2, '\0');
    size_t n = pp_seal_aad(m.c_str(), t.c_str(), buf.data(), buf.size());
    buf.resize(n);
    return buf;
}

bool parse_seal_header(std::string_view header, std::string &kid, unsigned char enc[32])
{
    char k[65];
    if (pp_seal_header_parse(std::string(header).c_str(), k, enc) != 0)
        return false;
    kid = k;
    return true;
}

bool answer_key(const Hpke &ctx, unsigned char key[32])
{
    return pp_seal_answer_key(pp(ctx), key) == 0;
}

bool seal_answer(const unsigned char key[32], int status, std::string_view pt, std::string &out)
{
    out.assign(pp_seal_answer_len(pt.size()), '\0');
    return pp_seal_answer(key, status, pt.data(), pt.size(), (unsigned char *)out.data()) == 0;
}

bool open_answer(const unsigned char key[32], int status, std::string_view ct, std::string &out)
{
    out.assign(pp_seal_plain_len(ct.size()) + 1, '\0');
    size_t n = 0;
    if (pp_open_answer(key, status, (const unsigned char *)ct.data(), ct.size(), (unsigned char *)out.data(), &n) != 0)
        return false;
    out.resize(n);
    return true;
}

bool pond_seal_key(const unsigned char seed[32], unsigned char sk[32], unsigned char pk[32])
{
    return pp_seal_key_from_seed(seed, PP_SEAL_INFO_POND, sk, pk) == 0;
}

} // namespace pond
