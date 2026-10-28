#pragma once

/* pond.yaml: where pondd listens, what its invite links say, where its files are and which database it uses
 * (docs/design.md, "pond.yaml"). pondd and pondctl read the same file. */

#include <string>

namespace pond {

struct DbConfig {
    std::string host = "127.0.0.1";
    unsigned port = 3306;
    std::string socket; /* a Unix socket instead of host and port */
    std::string user = "pond";
    std::string password;
    std::string name = "pond";
};

struct Config {
    std::string listen = "0.0.0.0";
    unsigned port = 8443;
    std::string address; /* host[:port] for invite links; "" = listen:port */
    std::string data_dir;
    DbConfig db;
};

/* Read `path`. false with the reason in `err`. Unknown keys are an error, so a typo is not silently ignored. */
bool load_config(const std::string &path, Config &out, std::string &err);

/* The address invite links carry: `address`, or listen:port when that is a real address. */
std::string link_address(const Config &c);

} // namespace pond
