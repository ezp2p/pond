#include "encoding.h"

#include <puddle_proto.h>

#include <openssl/rand.h>

#include <cstring>

/* The protocol's encodings are libpuddle-proto's (third_party/protocol/lib); these are the Pond's std::string faces on them. */

namespace pond {

std::string base32(const unsigned char *in, size_t len)
{
    std::string out((len * 8 + 4) / 5 + 1, '\0');
    out.resize(pp_base32_encode(in, len, out.data(), out.size()));
    return out;
}

std::string base64(const unsigned char *in, size_t len)
{
    std::string out((len + 2) / 3 * 4 + 1, '\0');
    out.resize(pp_base64_encode(in, len, out.data(), out.size()));
    return out;
}

bool unbase64(std::string_view in, unsigned char *out, size_t want)
{
    std::string s(in);
    std::string buf(want + 3, '\0');
    size_t n = 0;
    if (pp_base64_decode(s.c_str(), (unsigned char *)buf.data(), buf.size(), &n) != 0 || n != want)
        return false;
    memcpy(out, buf.data(), want);
    return true;
}

std::string hex(const unsigned char *in, size_t len)
{
    std::string out(2 * len + 1, '\0');
    pp_hex_encode(in, (unsigned)len, out.data());
    out.resize(2 * len);
    return out;
}

bool unhex(std::string_view in, unsigned char *out, size_t want)
{
    if (in.size() != 2 * want)
        return false;
    return pp_hex_decode(std::string(in).c_str(), out, want) == 0;
}

void sha256(const void *data, size_t len, unsigned char out[32])
{
    pp_sha256(data, len, out);
}

std::string sha256_hex(std::string_view data)
{
    char h[65];
    pp_sha256_hex(data.data(), data.size(), h);
    return h;
}

bool random_bytes(unsigned char *out, size_t len)
{
    return RAND_bytes(out, (int)len) == 1;
}

} // namespace pond
