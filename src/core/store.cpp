#include <redisx/core/store.hpp>
#include <functional>
#include <memory>

namespace redisx {

    // KV

    std::optional<std::string> Shard::get(const std::string& k) {
        auto now = std::chrono::steady_clock::now();
        std::unique_lock lk(mu_);
        if (is_expired_unlocked(k, now)) {
            map_.erase(k);
            ttl_.erase(k);
            hmap_.erase(k); // if key used as hash, expire it too
            return std::nullopt;
        }
        auto it = map_.find(k);
        if (it == map_.end()) return std::nullopt;
        return it->second;
    }

    void Shard::set(const std::string& k, std::string v) {
        auto now = std::chrono::steady_clock::now();
        std::unique_lock lk(mu_);
        if (is_expired_unlocked(k, now)) {
            ttl_.erase(k);
            hmap_.erase(k);
        }
        map_[k] = std::move(v);
        hmap_.erase(k);
    }

    bool Shard::del(const std::string& k) {
        std::unique_lock lk(mu_);
        ttl_.erase(k);
        bool s = map_.erase(k) > 0;
        bool h = hmap_.erase(k) > 0;
        return s || h;
    }

    // TTL

    void Shard::set_expire(const std::string& k, std::chrono::steady_clock::time_point tp) {
        std::unique_lock lk(mu_);
        // only set TTL if key exists (string or hash)
        if (map_.find(k) != map_.end() || hmap_.find(k) != hmap_.end()) {
            ttl_[k] = tp;
        }
    }

    long long Shard::ttl_ms(const std::string& k, std::chrono::steady_clock::time_point now) {
        std::shared_lock lk(mu_);
        bool exists = (map_.find(k) != map_.end()) || (hmap_.find(k) != hmap_.end());
        if (!exists) return -2;
        auto it = ttl_.find(k);
        if (it == ttl_.end()) return -1;
        auto remain = std::chrono::duration_cast<std::chrono::milliseconds>(it->second - now).count();
        if (remain <= 0) return -2;
        return remain;
    }

    void Shard::clear_expire(const std::string& k) {
        std::unique_lock lk(mu_);
        ttl_.erase(k);
    }

    bool Shard::is_expired_unlocked(const std::string& k, std::chrono::steady_clock::time_point now) const {
        auto it = ttl_.find(k);
        if (it == ttl_.end()) return false;
        return now >= it->second;
    }

    void Shard::sweep(std::chrono::steady_clock::time_point now) {
        std::unique_lock lk(mu_);
        std::vector<std::string> to_erase;
        to_erase.reserve(ttl_.size());
        for (auto& [key, tp] : ttl_) {
            if (now >= tp) to_erase.push_back(key);
        }
        for (auto& k : to_erase) {
            map_.erase(k);
            hmap_.erase(k);
            ttl_.erase(k);
        }
    }

    // Hashes

    // Make hset NOT try to overwrite a string, router will enforce WRONGTYPE before calling.
    int Shard::hset(const std::string& key, const std::string& field, const std::string& value) {
        auto now = std::chrono::steady_clock::now();
        std::unique_lock lk(mu_);
        if (is_expired_unlocked(key, now)) {
            ttl_.erase(key);
            map_.erase(key);
            hmap_.erase(key);
        }
        auto& hm = hmap_[key];
        auto it = hm.find(field);
        if (it == hm.end()) { hm.emplace(field, value); return 1; }
        it->second = value; return 0;
    }

    std::optional<std::string> Shard::hget(const std::string& key, const std::string& field) {
        auto now = std::chrono::steady_clock::now();
        std::shared_lock lk(mu_);
        if (is_expired_unlocked(key, now)) return std::nullopt;
        auto kh = hmap_.find(key);
        if (kh == hmap_.end()) return std::nullopt;
        auto it = kh->second.find(field);
        if (it == kh->second.end()) return std::nullopt;
        return it->second;
    }

    int Shard::hdel(const std::string& key, const std::string& field) {
        auto now = std::chrono::steady_clock::now();
        std::unique_lock lk(mu_);
        if (is_expired_unlocked(key, now)) {
            ttl_.erase(key);
            map_.erase(key);
            hmap_.erase(key);
            return 0;
        }
        auto kh = hmap_.find(key);
        if (kh == hmap_.end()) return 0;
        int removed = (int)(kh->second.erase(field) > 0);
        if (kh->second.empty()) {
            hmap_.erase(kh);
        }
        return removed;
    }

    int Shard::hexists(const std::string& key, const std::string& field) {
        auto now = std::chrono::steady_clock::now();
        std::shared_lock lk(mu_);
        if (is_expired_unlocked(key, now)) return 0;
        auto kh = hmap_.find(key);
        if (kh == hmap_.end()) return 0;
        return kh->second.count(field) ? 1 : 0;
    }

    long long Shard::hlen(const std::string& key) {
        auto now = std::chrono::steady_clock::now();
        std::shared_lock lk(mu_);
        if (is_expired_unlocked(key, now)) return 0;
        auto kh = hmap_.find(key);
        if (kh == hmap_.end()) return 0;
        return static_cast<long long>(kh->second.size());
    }

    std::vector<std::string> Shard::hgetall(const std::string& key) {
        auto now = std::chrono::steady_clock::now();
        std::shared_lock lk(mu_);
        std::vector<std::string> out;
        if (is_expired_unlocked(key, now)) return out;
        auto kh = hmap_.find(key);
        if (kh == hmap_.end()) return out;
        out.reserve(kh->second.size() * 2);
        for (auto& [f, v] : kh->second) {
            out.push_back(f);
            out.push_back(v);
        }
        return out;
    }

    // Store

    Store::Store(size_t n) {
        if (n == 0) n = 1;
        shards_.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            shards_.push_back(std::make_unique<Shard>());
        }
    }

    Shard& Store::shard_for(const std::string& key) {
        size_t h = std::hash<std::string>{}(key);
        return *shards_[h % shards_.size()];
    }

    void Store::sweep_all() {
        auto now = std::chrono::steady_clock::now();
        for (auto& s : shards_) s->sweep(now);
    }

    ValueType Shard::type_of(const std::string& key, std::chrono::steady_clock::time_point now) {
        std::unique_lock lk(mu_);
        if (is_expired_unlocked(key, now)) {
            // lazy expire: clear any data for this key
            map_.erase(key);
            hmap_.erase(key);
            ttl_.erase(key);
            return ValueType::None;
        }
        if (map_.find(key) != map_.end())  return ValueType::String;
        if (hmap_.find(key) != hmap_.end()) return ValueType::Hash;
        return ValueType::None;
    }

} // namespace redisx



