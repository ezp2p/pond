#include "config.h"

#include "core/yamltree.h"

#include <cstdlib>
#include <cstring>

namespace pond {

static bool scalar(const ynode *n, const char *key, std::string &out, std::string &err)
{
    const char *s = ynode_scalar(n);
    if (!s) {
        err = std::string(key) + " must be a single value (line " + std::to_string(ynode_line(n)) + ")";
        return false;
    }
    out = s;
    return true;
}

static bool port(const ynode *n, const char *key, unsigned &out, std::string &err)
{
    std::string s;
    if (!scalar(n, key, s, err))
        return false;
    char *end = nullptr;
    long v = strtol(s.c_str(), &end, 10);
    if (s.empty() || *end || v < 1 || v > 65535) {
        err = std::string(key) + " must be a port number (line " + std::to_string(ynode_line(n)) + ")";
        return false;
    }
    out = (unsigned)v;
    return true;
}

static bool database(const ynode *map, DbConfig &db, std::string &err)
{
    if (ynode_kind(map) != Y_MAP) {
        err = "database must be a map";
        return false;
    }
    for (size_t i = 0; i < ynode_count(map); i++) {
        std::string k = ynode_key(map, i);
        const ynode *v = ynode_item(map, i);
        bool ok = k == "host"       ? scalar(v, "database.host", db.host, err)
                  : k == "port"     ? port(v, "database.port", db.port, err)
                  : k == "socket"   ? scalar(v, "database.socket", db.socket, err)
                  : k == "user"     ? scalar(v, "database.user", db.user, err)
                  : k == "password" ? scalar(v, "database.password", db.password, err)
                  : k == "name"     ? scalar(v, "database.name", db.name, err)
                                    : (err = "unknown setting database." + k, false);
        if (!ok)
            return false;
    }
    return true;
}

bool load_config(const std::string &path, Config &out, std::string &err)
{
    char e[256] = "";
    ynode *root = puddle_yaml_parse_file(path.c_str(), e, sizeof e);
    if (!root) {
        err = e;
        return false;
    }
    bool ok = true;
    if (ynode_kind(root) != Y_MAP) {
        err = path + ": not a map of settings";
        ok = false;
    }
    for (size_t i = 0; ok && i < ynode_count(root); i++) {
        std::string k = ynode_key(root, i);
        const ynode *v = ynode_item(root, i);
        ok = k == "listen"     ? scalar(v, "listen", out.listen, err)
             : k == "port"     ? port(v, "port", out.port, err)
             : k == "address"  ? scalar(v, "address", out.address, err)
             : k == "data_dir" ? scalar(v, "data_dir", out.data_dir, err)
             : k == "database" ? database(v, out.db, err)
                               : (err = "unknown setting " + k, false);
    }
    puddle_yaml_free(root);
    if (ok && out.data_dir.empty()) {
        err = "data_dir is required";
        ok = false;
    }
    if (ok && !err.empty())
        err.clear();
    if (!ok && err.rfind(path, 0) != 0)
        err = path + ": " + err;
    return ok;
}

std::string link_address(const Config &c)
{
    if (!c.address.empty())
        return c.address;
    std::string host = c.listen == "0.0.0.0" || c.listen == "::" ? "127.0.0.1" : c.listen;
    if (host.find(':') != std::string::npos)
        host = "[" + host + "]";
    return c.port == 443 ? host : host + ":" + std::to_string(c.port);
}

} // namespace pond
