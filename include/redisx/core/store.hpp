#pragma once
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <memory>
#include <chrono>
#include <optional>

namespace redisx {

	enum class ValueType { None, String, Hash };

	class Shard {
	public:
		Shard() = default;
		Shard(const Shard&) = delete;
		Shard& operator=(const Shard&) = delete;
		Shard(Shard&&) = delete;
		Shard& operator=(Shard&&) = delete; 

		// KV
		std::optional<std::string> get(const std::string& k);
		void set(const std::string& k, std::string v);
		bool del(const std::string& k);

		// TTL
		void set_expire(const std::string& k, std::chrono::steady_clock::time_point tp);
		long long ttl_ms(const std::string& k, std::chrono::steady_clock::time_point now);
		void clear_expire(const std::string& k);
		void sweep(std::chrono::steady_clock::time_point now);

		// Stores key current value type (treats expired as None)
		ValueType type_of(const std::string& key, std::chrono::steady_clock::time_point now);

		// HASHES (all return Redis-like integers/bulk semantics)

		// HSET key field value -> returns 1 if new field, 0 if updated
		int hset(const std::string& key, const std::string& field, const std::string& value);
		// HGET key field -> optional value
		std::optional<std::string> hget(const std::string& key, const std::string& field);
		// HDEL key field -> returns #fields removed (0 or 1 here)
		int hdel(const std::string& key, const std::string& field);
		// HEXISTS key field -> 1/0
		int hexists(const std::string& key, const std::string& field);
		// HLEN key -> number of fields
		long long hlen(const std::string& key);
		// HGETALL key -> vector of [field, value, field, value, ...]
		std::vector<std::string> hgetall(const std::string& key);

	private:
		bool is_expired_unlocked(const std::string& k, std::chrono::steady_clock::time_point now) const;

		mutable std::shared_mutex mu_;
		// String keys
		std::unordered_map<std::string, std::string> map_;
		// Key -> expire time
		std::unordered_map<std::string, std::chrono::steady_clock::time_point> ttl_;
		// Hash keys: key -> (field -> value)
		std::unordered_map<std::string, std::unordered_map<std::string, std::string>> hmap_;
	};

	class Store {
	public:
		explicit Store(size_t n_shards = 1);
		Shard& shard_for(const std::string& key);
		Shard& shard_by_index(size_t i) { return *shards_[i]; }
		size_t shard_count() const { return shards_.size(); }
		void sweep_all();

	private:
		std::vector<std::unique_ptr<Shard>> shards_;
	};

} // namespace redisx




