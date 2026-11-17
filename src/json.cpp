#include "json.h"

#include <cstdio>
#include <cstring>
#include <memory>

namespace pond {

std::string jstr(std::string_view s)
{
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') {
            o += '\\';
            o += (char)c;
        } else if (c == '\n') {
            o += "\\n";
        } else if (c < 0x20) {
            char u[8];
            snprintf(u, sizeof u, "\\u%04x", c);
            o += u;
        } else {
            o += (char)c;
        }
    }
    return o + "\"";
}

bool parse_object(std::string_view text, Json::Value &out)
{
    static thread_local const std::unique_ptr<Json::CharReader> reader = [] { /* a reader is not for two threads at once */
        Json::CharReaderBuilder b;
        Json::CharReaderBuilder::strictMode(&b.settings_);
        b.settings_["stackLimit"] = 64;
        return std::unique_ptr<Json::CharReader>(b.newCharReader());
    }();
    return reader->parse(text.data(), text.data() + text.size(), &out, nullptr) && out.isObject();
}

static size_t skip_ws(std::string_view s, size_t i)
{
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r'))
        i++;
    return i;
}

/* The end of the JSON value starting at `i` (one past it), in a document already known to be well formed. */
static size_t value_end(std::string_view s, size_t i)
{
    if (s[i] == '"') {
        for (i++; i < s.size() && s[i] != '"'; i++)
            if (s[i] == '\\')
                i++;
        return i + 1;
    }
    if (s[i] == '{' || s[i] == '[') {
        int depth = 0;
        for (; i < s.size(); i++) {
            char c = s[i];
            if (c == '"')
                i = value_end(s, i) - 1;
            else if (c == '{' || c == '[')
                depth++;
            else if ((c == '}' || c == ']') && --depth == 0)
                return i + 1;
        }
        return s.size();
    }
    while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']' && s[i] != ' ' && s[i] != '\n' && s[i] != '\r' && s[i] != '\t')
        i++;
    return i;
}

bool raw_member(std::string_view obj, std::string_view key, std::string_view &out)
{
    size_t i = skip_ws(obj, 0);
    if (i >= obj.size() || obj[i] != '{')
        return false;
    i = skip_ws(obj, i + 1);
    while (i < obj.size() && obj[i] == '"') {
        size_t kend = value_end(obj, i);
        std::string_view k = obj.substr(i + 1, kend - i - 2);
        i = skip_ws(obj, kend);
        if (i >= obj.size() || obj[i] != ':')
            return false;
        i = skip_ws(obj, i + 1);
        size_t vend = value_end(obj, i);
        if (k == key) {
            out = obj.substr(i, vend - i);
            return true;
        }
        i = skip_ws(obj, vend);
        if (i < obj.size() && obj[i] == ',')
            i = skip_ws(obj, i + 1);
    }
    return false;
}

bool get_string(const Json::Value &obj, const char *key, std::string &out)
{
    const Json::Value *v = obj.find(key, key + strlen(key));
    if (!v || !v->isString())
        return false;
    out = v->asString();
    return true;
}

} // namespace pond
