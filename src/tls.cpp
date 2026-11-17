#include "tls.h"

#include "encoding.h"

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>

#include <cstdio>
#include <filesystem>
#include <sys/stat.h>

namespace pond {

static bool add_ext(X509 *x, int nid, const char *value)
{
    X509V3_CTX ctx;
    X509V3_set_ctx_nodb(&ctx);
    X509V3_set_ctx(&ctx, x, x, nullptr, nullptr, 0);
    X509_EXTENSION *ext = X509V3_EXT_conf_nid(nullptr, &ctx, nid, value);
    bool ok = ext && X509_add_ext(x, ext, -1) == 1;
    X509_EXTENSION_free(ext);
    return ok;
}

/* Write `pem` to `path`, readable only by its owner. */
static bool write_private(const std::string &path, BIO *pem)
{
    char *data = nullptr;
    long len = BIO_get_mem_data(pem, &data);
    FILE *f = fopen(path.c_str(), "wx");
    if (!f)
        return false;
    chmod(path.c_str(), 0600);
    bool ok = fwrite(data, 1, (size_t)len, f) == (size_t)len;
    return fclose(f) == 0 && ok;
}

bool ensure_certificate(const std::string &dir, std::string &err)
{
    namespace fs = std::filesystem;
    std::string cert = dir + "/cert.pem", key = dir + "/key.pem";
    bool have_cert = fs::exists(cert), have_key = fs::exists(key);
    if (have_cert && have_key)
        return true;
    if (have_cert != have_key) {
        err = "only one of " + cert + " and " + key + " exists; put back the other, or remove both to make a new certificate";
        return false;
    }
    EVP_PKEY *pkey = EVP_EC_gen("P-256");
    X509 *x = X509_new();
    BIO *kp = BIO_new(BIO_s_mem()), *cp = BIO_new(BIO_s_mem());
    unsigned char sb[8];
    bool ok = pkey && x && kp && cp && random_bytes(sb, sizeof sb);
    if (ok) {
        sb[0] = (unsigned char)((sb[0] & 0x7f) | 0x40); /* positive, non-zero */
        uint64_t serial = 0;
        for (unsigned char b : sb)
            serial = serial << 8 | b;
        X509_NAME *name = X509_NAME_new();
        ok = name && X509_set_version(x, 2) && ASN1_INTEGER_set_uint64(X509_get_serialNumber(x), serial) &&
             X509_gmtime_adj(X509_getm_notBefore(x), -3600) && X509_gmtime_adj(X509_getm_notAfter(x), 3650L * 24 * 3600) &&
             X509_set_pubkey(x, pkey) && X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (const unsigned char *)"pond", -1, -1, 0) &&
             X509_set_subject_name(x, name) && X509_set_issuer_name(x, name) && add_ext(x, NID_basic_constraints, "critical,CA:FALSE") &&
             add_ext(x, NID_key_usage, "critical,digitalSignature") && add_ext(x, NID_ext_key_usage, "serverAuth") &&
             add_ext(x, NID_subject_alt_name, "DNS:localhost,IP:127.0.0.1,IP:::1") && X509_sign(x, pkey, EVP_sha256()) > 0 &&
             PEM_write_bio_PrivateKey(kp, pkey, nullptr, nullptr, 0, nullptr, nullptr) && PEM_write_bio_X509(cp, x);
        X509_NAME_free(name);
    }
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ok && !(write_private(key, kp) && write_private(cert, cp))) {
        fs::remove(key, ec);
        fs::remove(cert, ec);
        err = "cannot write " + cert + " and " + key;
        ok = false;
    } else if (!ok) {
        err = "cannot make a certificate";
    }
    BIO_free(kp);
    BIO_free(cp);
    X509_free(x);
    EVP_PKEY_free(pkey);
    return ok;
}

std::string certificate_fingerprint(const std::string &cert_path)
{
    FILE *f = fopen(cert_path.c_str(), "r");
    if (!f)
        return "";
    X509 *x = PEM_read_X509(f, nullptr, nullptr, nullptr);
    fclose(f);
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned n = 0;
    std::string out = x && X509_digest(x, EVP_sha256(), md, &n) && n == 32 ? hex(md, 32) : "";
    X509_free(x);
    return out;
}

} // namespace pond
