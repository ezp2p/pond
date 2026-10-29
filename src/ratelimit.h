#pragma once

/* Token buckets: `burst` at once, refilled at one per `every_ms`. A Bucket is one shared limit (joins from strangers); Keyed
 * is one limit per key (sends per member), forgetting keys that have been idle long enough to be full again. */

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace pond {

class Bucket {
public:
    Bucket(double burst, int64_t every_ms) : burst_(burst), every_ms_(every_ms), tokens_(burst) {}
    /* Take one token at `now_ms`; false if there is none. */
    bool take(int64_t now_ms);

private:
    friend class Keyed;
    double burst_;
    int64_t every_ms_;
    double tokens_;
    int64_t at_ = 0;
};

class Keyed {
public:
    Keyed(double burst, int64_t every_ms, size_t max_keys = 1 << 16) : burst_(burst), every_ms_(every_ms), max_keys_(max_keys) {}
    bool take(const std::string &key, int64_t now_ms);

private:
    double burst_;
    int64_t every_ms_;
    size_t max_keys_;
    std::mutex mu_;
    std::unordered_map<std::string, Bucket> buckets_;
};

} // namespace pond
