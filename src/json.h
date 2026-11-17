#pragma once

/* JSON as the Pond reads and writes it. Bodies are parsed strictly with jsoncpp. Two things need the exact bytes a member
 * sent, not a re-serialisation: a key chain's records, whose signatures cover their bytes, and a profile, which is kept as
 * received. raw_member() finds those. */

#include <json/json.h>

#include <string>
#include <string_view>

namespace pond {

/* `s` as a JSON string, quotes included. */
std::string jstr(std::string_view s);

/* Parse `text` as one JSON object: no comments, no trailing data, no duplicate keys. */
bool parse_object(std::string_view text, Json::Value &out);

/* The exact text of the value of `key` in the JSON object `obj`, which must be well formed (parse_object first). Only the
 * object's own members are looked at, not nested ones. false if there is no such member. */
bool raw_member(std::string_view obj, std::string_view key, std::string_view &out);

/* A string member, false if it is missing or not a string. */
bool get_string(const Json::Value &obj, const char *key, std::string &out);

} // namespace pond
