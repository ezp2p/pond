#pragma once

/* The Pond's TLS certificate: <data_dir>/cert.pem and key.pem. pondd makes a self-signed one the first time if neither is
 * there; the admin may put their own there instead. Members' Puddles pin the certificate they saw when they joined, by the
 * SHA-256 of its DER encoding. */

#include <string>

namespace pond {

/* Make cert.pem and key.pem in `dir` unless both exist. false with the reason in `err` (also if only one exists). */
bool ensure_certificate(const std::string &dir, std::string &err);

/* The SHA-256 of the first certificate in `cert_path`, as 64 lowercase hex characters; "" if it can't be read. */
std::string certificate_fingerprint(const std::string &cert_path);

} // namespace pond
