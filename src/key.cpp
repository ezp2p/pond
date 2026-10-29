#include "key.h"

#include "encoding.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>

namespace pond {

static bool public_of(const unsigned char seed[32], PondKey &out, std::string &err)
{
    EVP_PKEY *k = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, seed, 32);
    size_t len = 32;
    bool ok = k && EVP_PKEY_get_raw_public_key(k, out.pub, &len) == 1 && len == 32;
    EVP_PKEY_free(k);
    if (!ok) {
        err = "cannot make the Pond's key";
        return false;
    }
    unsigned char h[32];
    sha256(out.pub, 32, h);
    out.id = base32(h, 32);
    return true;
}

bool create_key(const std::string &data_dir, PondKey &out, std::string &err)
{
    std::string path = data_dir + "/pond.key";
    std::error_code ec;
    std::filesystem::create_directories(data_dir, ec);
    if (ec) {
        err = "cannot make " + data_dir;
        return false;
    }
    unsigned char seed[32];
    if (!random_bytes(seed, sizeof seed) || !public_of(seed, out, err)) {
        err = err.empty() ? "no randomness" : err;
        return false;
    }
    std::string line = hex(seed, 32) + "\n";
    OPENSSL_cleanse(seed, sizeof seed);
    FILE *f = fopen(path.c_str(), "wx"); /* never over a key that is there: a Pond with a new key is a different Pond */
    if (!f) {
        OPENSSL_cleanse(line.data(), line.size());
        err = std::filesystem::exists(path) ? path + " already exists; this Pond already has a key" : "cannot write " + path;
        return false;
    }
    chmod(path.c_str(), 0600);
    bool ok = fwrite(line.data(), 1, line.size(), f) == line.size() && fflush(f) == 0 && fsync(fileno(f)) == 0;
    ok = fclose(f) == 0 && ok;
    OPENSSL_cleanse(line.data(), line.size());
    if (!ok) {
        std::filesystem::remove(path, ec);
        err = "cannot write " + path;
    }
    return ok;
}

bool load_key(const std::string &data_dir, PondKey &out, std::string &err)
{
    std::string path = data_dir + "/pond.key";
    std::ifstream f(path);
    std::string line;
    if (!f || !std::getline(f, line)) {
        err = "no key at " + path + " (run pondctl init)";
        return false;
    }
    unsigned char seed[32];
    bool ok = unhex(line.substr(0, 64), seed, 32);
    OPENSSL_cleanse(line.data(), line.size());
    if (!ok) {
        err = path + " is damaged";
        return false;
    }
    ok = public_of(seed, out, err);
    OPENSSL_cleanse(seed, sizeof seed);
    return ok;
}

} // namespace pond
