#pragma once

/* Text from members and the admin, made safe to keep and show: valid UTF-8 (a bad byte becomes U+FFFD), no control
 * characters, line breaks only where `multiline` allows them (else they become spaces), and no whitespace at either end. */

#include <cstddef>
#include <string>
#include <string_view>

namespace pond {

/* false if the result would be longer than `max` characters. An empty result is allowed: the caller decides. */
bool clean_text(std::string_view in, size_t max, bool multiline, std::string &out);

} // namespace pond
