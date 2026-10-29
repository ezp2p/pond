#include "encoding.h"

#include <openssl/evp.h>
#include <openssl/rand.h>

namespace pond {

std::string base32(const unsigned char *in, size_t len)
{
    static const char alpha[] = "abcdefghijklmnopqrstuvwxyz234567";
    std::string out;
    uint32_t buf = 0;
    int bits = 0;
    for (size_t i = 0; i < len; i++) {
        buf = (buf << 8) | in[i];
        bits += 8;
        while (bits >= 5) {
            out += alpha[(buf >> (bits - 5)) & 31];
            bits -= 5;
        }
    }
    if (bits > 0)
        out += alpha[(buf << (5 - bits)) & 31];
    return out;
}

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64(const unsigned char *in, size_t len)
{
    std::string out;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < len ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < len ? in[i + 2] : 0);
        out += B64[v >> 18 & 63];
        out += B64[v >> 12 & 63];
        out += i + 1 < len ? B64[v >> 6 & 63] : '=';
        out += i + 2 < len ? B64[v & 63] : '=';
    }
    return out;
}

static int b64_value(char c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    return c == '+' ? 62 : c == '/' ? 63 : -1;
}

bool unbase64(std::string_view in, unsigned char *out, size_t want)
{
    if (in.size() % 4 != 0 || in.size() / 4 * 3 < want || in.size() / 4 * 3 >= want + 3)
        return false;
    size_t pad = in.size() / 4 * 3 - want; /* 0, 1 or 2 '=' at the end */
    size_t n = 0;
    for (size_t i = 0; i < in.size(); i += 4) {
        int v[4];
        for (int k = 0; k < 4; k++) {
            bool padding = i + 4 == in.size() && k >= 4 - (int)pad;
            v[k] = padding ? (in[i + k] == '=' ? 0 : -1) : b64_value(in[i + k]);
            if (v[k] < 0)
                return false;
        }
        uint32_t w = (uint32_t)v[0] << 18 | (uint32_t)v[1] << 12 | (uint32_t)v[2] << 6 | (uint32_t)v[3];
        unsigned char b[3] = {(unsigned char)(w >> 16), (unsigned char)(w >> 8), (unsigned char)w};
        for (int k = 0; k < 3; k++) {
            if (n < want)
                out[n++] = b[k];
            else if (b[k]) /* bits past the end must be zero */
                return false;
        }
    }
    return n == want;
}

std::string hex(const unsigned char *in, size_t len)
{
    static const char digits[] = "0123456789abcdef";
    std::string out;
    for (size_t i = 0; i < len; i++) {
        out += digits[in[i] >> 4];
        out += digits[in[i] & 15];
    }
    return out;
}

bool unhex(std::string_view in, unsigned char *out, size_t want)
{
    if (in.size() != want * 2)
        return false;
    auto v = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
    for (size_t i = 0; i < want; i++) {
        int hi = v(in[2 * i]), lo = v(in[2 * i + 1]);
        if (hi < 0 || lo < 0)
            return false;
        out[i] = (unsigned char)(hi << 4 | lo);
    }
    return true;
}

void sha256(const void *data, size_t len, unsigned char out[32])
{
    unsigned n = 32;
    EVP_Digest(data, len, out, &n, EVP_sha256(), nullptr);
}

std::string sha256_hex(std::string_view data)
{
    unsigned char h[32];
    sha256(data.data(), data.size(), h);
    return hex(h, 32);
}

bool random_bytes(unsigned char *out, size_t len)
{
    return RAND_bytes(out, (int)len) == 1;
}

} // namespace pond
