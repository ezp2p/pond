#include "config.h"

#include <yaml-cpp/yaml.h>

#include <cstdlib>

namespace pond {

static std::string where(const YAML::Node &n)
{
    return " (line " + std::to_string(n.Mark().line + 1) + ")";
}

static bool scalar(const YAML::Node &n, const std::string &key, std::string &out, std::string &err)
{
    if (!n.IsScalar()) {
        err = key + " must be a single value" + where(n);
        return false;
    }
    out = n.Scalar();
    return true;
}

static bool number(const YAML::Node &n, const std::string &key, long lo, long hi, long &out, std::string &err)
{
    std::string s;
    if (!scalar(n, key, s, err))
        return false;
    char *end = nullptr;
    out = strtol(s.c_str(), &end, 10);
    if (s.empty() || *end || out < lo || out > hi) {
        err = key + " must be a number from " + std::to_string(lo) + " to " + std::to_string(hi) + where(n);
        return false;
    }
    return true;
}

static bool number(const YAML::Node &n, const std::string &key, long lo, long hi, unsigned &out, std::string &err)
{
    long v;
    return number(n, key, lo, hi, v, err) && (out = (unsigned)v, true);
}

static bool database(const YAML::Node &map, DbConfig &db, std::string &err)
{
    if (!map.IsMap()) {
        err = "database must be a map" + where(map);
        return false;
    }
    for (auto it : map) {
        std::string k = it.first.as<std::string>();
        const YAML::Node &v = it.second;
        bool ok = k == "host"          ? scalar(v, "database.host", db.host, err)
                  : k == "port"        ? number(v, "database.port", 1, 65535, db.port, err)
                  : k == "user"        ? scalar(v, "database.user", db.user, err)
                  : k == "password"    ? scalar(v, "database.password", db.password, err)
                  : k == "name"        ? scalar(v, "database.name", db.name, err)
                  : k == "connections" ? number(v, "database.connections", 1, 1000, db.connections, err)
                  : k == "socket"      ? (err = "database.socket is no longer used: set host to localhost for the default socket", false)
                                       : (err = "unknown setting database." + k, false);
        if (!ok)
            return false;
    }
    for (const std::string *v : {&db.host, &db.user, &db.name})
        if (v->empty() || v->find_first_of(" \t'\\") != std::string::npos) {
            err = "database host, user and name must not be empty or have spaces, quotes or backslashes in them";
            return false;
        }
    return true;
}

bool load_config(const std::string &path, Config &out, std::string &err)
{
    YAML::Node root;
    try {
        root = YAML::LoadFile(path);
    } catch (const YAML::Exception &e) {
        err = path + ": " + e.what();
        return false;
    }
    bool ok = true;
    if (!root.IsMap()) {
        err = "not a map of settings";
        ok = false;
    }
    for (auto it = root.begin(); ok && it != root.end(); ++it) {
        std::string k = it->first.as<std::string>();
        const YAML::Node &v = it->second;
        long wait;
        ok = k == "listen"           ? scalar(v, "listen", out.listen, err)
             : k == "port"           ? number(v, "port", 1, 65535, out.port, err)
             : k == "address"        ? scalar(v, "address", out.address, err)
             : k == "data_dir"       ? scalar(v, "data_dir", out.data_dir, err)
             : k == "threads"        ? number(v, "threads", 0, 1024, out.threads, err)
             : k == "events_wait_ms" ? number(v, "events_wait_ms", 100, 60000, wait, err) && (out.events_wait_ms = (int)wait, true)
             : k == "database"       ? database(v, out.db, err)
                                     : (err = "unknown setting " + k, false);
    }
    if (ok && out.data_dir.empty()) {
        err = "data_dir is required";
        ok = false;
    }
    if (!ok)
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
