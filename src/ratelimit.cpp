#include "ratelimit.h"

#include <algorithm>

namespace pond {

bool Bucket::take(int64_t now)
{
    if (at_ && now > at_)
        tokens_ = std::min(burst_, tokens_ + (double)(now - at_) / (double)every_ms_);
    at_ = now;
    if (tokens_ < 1)
        return false;
    tokens_ -= 1;
    return true;
}

bool Keyed::take(const std::string &key, int64_t now)
{
    std::lock_guard<std::mutex> g(mu_);
    if (buckets_.size() >= max_keys_) { /* drop the ones that have refilled: they hold nothing a new bucket would not */
        int64_t full_after = (int64_t)(burst_ * (double)every_ms_);
        std::erase_if(buckets_, [&](const auto &kv) { return now - kv.second.at_ >= full_after; });
        if (buckets_.size() >= max_keys_)
            return false;
    }
    auto it = buckets_.try_emplace(key, burst_, every_ms_).first;
    return it->second.take(now);
}

} // namespace pond
