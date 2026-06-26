/**
 * @file InMemoryCache.hpp
 * @brief In-memory CacheManager fake — no Redis. The DI seam that lets
 *        cache-aside / lock / fail-open logic be unit-tested without live infra:
 *
 *            Cache::install_for_testing(std::make_unique<TestSupport::InMemoryCache>());
 *            ... exercise code that calls Cache::get() ...
 *            Cache::reset_for_testing();
 *
 *        TTLs are accepted but not expired (unit tests don't sleep). For the
 *        same pattern over the data layer, give a repository an interface and
 *        pass a fake — see docs/EXAMPLES.md.
 */

#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "cache/Cache.hpp"

namespace TestSupport {

class InMemoryCache : public Cache::CacheManager {
public:
    bool is_initialized() const override { return true; }

    bool set(const std::string& k, const std::string& v, long /*ttl*/ = 0) override {
        store_[k] = v;
        return true;
    }

    bool set_nx(const std::string& k, const std::string& v, std::chrono::milliseconds /*ttl*/) override {
        return store_.emplace(k, v).second;  // true only on first insert (NX semantics)
    }

    std::optional<std::string> get(const std::string& k) override {
        auto it = store_.find(k);
        return it == store_.end() ? std::nullopt : std::optional<std::string>{it->second};
    }

    long del(const std::string& k) override { return static_cast<long>(store_.erase(k)); }

    long del(const std::vector<std::string>& keys) override {
        long n = 0;
        for (const auto& k : keys)
            n += static_cast<long>(store_.erase(k));
        return n;
    }

    bool exists(const std::string& k) override { return store_.count(k) > 0; }
    bool expire(const std::string&, long) override { return true; }
    long ttl(const std::string& k) override { return store_.count(k) ? -1 : -2; }

    long long incr(const std::string& k, long long by = 1) override {
        long long v = 0;
        auto it = store_.find(k);
        if (it != store_.end())
            v = std::stoll(it->second);
        v += by;
        store_[k] = std::to_string(v);
        return v;
    }
    long long decr(const std::string& k, long long by = 1) override { return incr(k, -by); }

private:
    std::unordered_map<std::string, std::string> store_;
};

}  // namespace TestSupport
