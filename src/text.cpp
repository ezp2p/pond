#include "text.h"

#include <cstdint>

namespace pond {

/* The next code point of `s` from `i`, moving `i` past it; U+FFFD for a byte that does not start a valid sequence. */
static uint32_t next_char(std::string_view s, size_t &i)
{
    unsigned char c = (unsigned char)s[i];
    int len = c < 0x80 ? 1 : (c & 0xe0) == 0xc0 ? 2 : (c & 0xf0) == 0xe0 ? 3 : (c & 0xf8) == 0xf0 ? 4 : 0;
    uint32_t cp = len == 1 ? c : len == 2 ? c & 0x1f : len == 3 ? c & 0x0f : c & 0x07;
    if (len == 0 || i + len > s.size()) {
        i++;
        return 0xfffd;
    }
    for (int k = 1; k < len; k++) {
        unsigned char b = (unsigned char)s[i + k];
        if ((b & 0xc0) != 0x80) {
            i++;
            return 0xfffd;
        }
        cp = cp << 6 | (b & 0x3f);
    }
    static const uint32_t least[] = {0, 0, 0x80, 0x800, 0x10000};
    if (cp < least[len] || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) { /* overlong, too large, or a surrogate */
        i++;
        return 0xfffd;
    }
    i += len;
    return cp;
}

static void put_char(std::string &out, uint32_t cp)
{
    if (cp < 0x80) {
        out += (char)cp;
    } else if (cp < 0x800) {
        out += (char)(0xc0 | cp >> 6);
        out += (char)(0x80 | (cp & 0x3f));
    } else if (cp < 0x10000) {
        out += (char)(0xe0 | cp >> 12);
        out += (char)(0x80 | (cp >> 6 & 0x3f));
        out += (char)(0x80 | (cp & 0x3f));
    } else {
        out += (char)(0xf0 | cp >> 18);
        out += (char)(0x80 | (cp >> 12 & 0x3f));
        out += (char)(0x80 | (cp >> 6 & 0x3f));
        out += (char)(0x80 | (cp & 0x3f));
    }
}

bool clean_text(std::string_view in, size_t max, bool multiline, std::string &out)
{
    std::string s;
    size_t chars = 0, i = 0;
    while (i < in.size()) {
        uint32_t cp = next_char(in, i);
        if (cp == '\r') { /* \r\n and a lone \r are both one line break */
            if (i < in.size() && in[i] == '\n')
                i++;
            cp = '\n';
        }
        if (cp == '\n' || cp == '\t') {
            cp = cp == '\n' && multiline ? '\n' : ' ';
        } else if (cp < 0x20 || (cp >= 0x7f && cp < 0xa0) || cp == 0x2028 || cp == 0x2029) {
            continue; /* control characters, and the Unicode line and paragraph separators */
        }
        put_char(s, cp);
        chars++;
    }
    /* trim; the whitespace removed is ASCII, so counting it back out is exact */
    size_t a = s.find_first_not_of(" \n"), b = s.find_last_not_of(" \n");
    if (a == std::string::npos) {
        out.clear();
        return true;
    }
    chars -= a + (s.size() - 1 - b);
    if (chars > max)
        return false;
    out = s.substr(a, b - a + 1);
    return true;
}

} // namespace pond
