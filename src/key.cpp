#include "key.h"

#include "base/encoding.h"
#include "base/hash.h"
#include "platform/fsutil.h"

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <cstring>

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
    char id[64];
    puddle_sha256(out.pub, 32, h);
    puddle_base32_encode(h, 32, id, sizeof id);
    out.id = id;
    return true;
}

bool create_key(const std::string &data_dir, PondKey &out, std::string &err)
{
    std::string path = data_dir + "/pond.key";
    if (puddle_mkdir_p(data_dir.c_str()) != 0) {
        err = "cannot make " + data_dir;
        return false;
    }
    if (puddle_path_exists(path.c_str())) {
        err = path + " already exists; this Pond already has a key";
        return false;
    }
    unsigned char seed[32];
    if (RAND_bytes(seed, sizeof seed) != 1 || !public_of(seed, out, err)) {
        err = err.empty() ? "no randomness" : err;
        return false;
    }
    char hex[65];
    puddle_hex_encode(seed, 32, hex);
    OPENSSL_cleanse(seed, sizeof seed);
    std::string line = std::string(hex) + "\n";
    OPENSSL_cleanse(hex, sizeof hex);
    int fd = puddle_fd_open(path.c_str(), PUDDLE_F_WRITE | PUDDLE_F_CREATE | PUDDLE_F_EXCL);
    bool ok = fd >= 0 && puddle_fd_write_all(fd, line.data(), line.size()) == 0 && puddle_fsync_fd(fd) == 0;
    if (fd >= 0)
        puddle_fd_close(fd);
    OPENSSL_cleanse(line.data(), line.size());
    if (!ok) {
        puddle_remove_file(path.c_str());
        err = "cannot write " + path;
    }
    return ok;
}

bool load_key(const std::string &data_dir, PondKey &out, std::string &err)
{
    std::string path = data_dir + "/pond.key";
    int fd = puddle_fd_open(path.c_str(), PUDDLE_F_READ);
    if (fd < 0) {
        err = "no key at " + path + " (run pondctl init)";
        return false;
    }
    char hex[80] = "";
    int64_t n = puddle_fd_read(fd, hex, sizeof hex - 1);
    puddle_fd_close(fd);
    unsigned char seed[32];
    bool ok = n >= 64 && (hex[64] = 0, puddle_hex_decode(hex, seed, 32) == 0);
    OPENSSL_cleanse(hex, sizeof hex);
    if (!ok) {
        err = path + " is damaged";
        return false;
    }
    ok = public_of(seed, out, err);
    OPENSSL_cleanse(seed, sizeof seed);
    return ok;
}

} // namespace pond
