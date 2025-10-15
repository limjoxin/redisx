#include <redisx/core/router.hpp>
#include <redisx/proto/resp.hpp>
#include <algorithm>
#include <chrono>


namespace redisx {

    static std::string upper(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::toupper(c); });
        return s;
    }

    static inline std::string resp_wrongtype() {
        return "-WRONGTYPE Operation against a key holding the wrong kind of value\r\n";
    }


    Router::Router(Store& s) : store_(s) {
        h_["PING"] = [](auto const& a) {
            if (a.size() > 1) return resp_bulk(a[1]);
            return resp_simple("PONG");
            };

        h_["ECHO"] = [](auto const& a) {
            if (a.size() < 2) return resp_error("wrong #args for 'echo'");
            return resp_bulk(a[1]);
            };

        h_["SET"] = [this](auto const& a) {
            if (a.size() < 3) return resp_error("wrong #args for 'set'");
            store_.shard_for(a[1]).set(a[1], a[2]);
            return resp_simple("OK");
            };

        h_["GET"] = [this](auto const& a) {
            if (a.size() < 2) return resp_error("wrong #args for 'get'");
            const std::string& key = a[1];
            auto& sh = store_.shard_for(key);
            auto now = std::chrono::steady_clock::now();

            auto t = sh.type_of(key, now);
            if (t == ValueType::Hash) return resp_wrongtype();

            auto v = sh.get(key);                                   // lazily evicts expired
            if (!v) return resp_nil();
            return resp_bulk(*v);
            };


        h_["DEL"] = [this](auto const& a) {
            if (a.size() < 2) return resp_error("wrong #args for 'del'");
            bool ok = store_.shard_for(a[1]).del(a[1]);
            return resp_int(ok ? 1 : 0);
            };

        h_["EXPIRE"] = [this](auto const& a) {
            // EXPIRE key seconds  -> returns 1 if TTL set, 0 otherwise
            if (a.size() < 3) return resp_error("wrong number of arguments for 'expire'");
            const std::string& key = a[1];
            long long sec = 0;
            try { sec = std::stoll(a[2]); }
            catch (...) { return resp_error("value is not an integer or out of range"); }
            if (sec < 0) sec = 0;
            auto& sh = store_.shard_for(key);
            // check existence by get (which also lazily evicts)
            auto cur = sh.get(key);
            if (!cur) return resp_int(0);
            auto tp = std::chrono::steady_clock::now() + std::chrono::seconds(sec);
            sh.set_expire(key, tp);
            return resp_int(1);
            };

        h_["TTL"] = [this](auto const& a) {
            if (a.size() < 2) return resp_error("wrong number of arguments for 'ttl'");
            const std::string& key = a[1];
            auto& sh = store_.shard_for(key);
            auto now = std::chrono::steady_clock::now();
            long long ms = sh.ttl_ms(key, now);
            if (ms == -2) return resp_int(-2);
            if (ms == -1) return resp_int(-1);
            long long secs = (ms + 999) / 1000; // ceil ms -> s
            return resp_int(secs);
            };

        // SET with EX/PX (only EX or PX, not both)
        h_["SET"] = [this](auto const& a) {
            if (a.size() < 3) return resp_error("wrong #args for 'set'");
            const std::string& key = a[1];
            const std::string& val = a[2];

            // parse optional EX/PX
            long long ttl_ms = -1;
            if (a.size() >= 5) {
                // pattern: SET k v EX 10  |  SET k v PX 1500
                std::string opt = a[3];
                std::transform(opt.begin(), opt.end(), opt.begin(), ::toupper);
                if (opt == "EX") {
                    try { ttl_ms = std::stoll(a[4]) * 1000; }
                    catch (...) { return resp_error("value is not an integer or out of range"); }
                }
                else if (opt == "PX") {
                    try { ttl_ms = std::stoll(a[4]); }
                    catch (...) { return resp_error("value is not an integer or out of range"); }
                }
                else {
                    return resp_error("syntax error");
                }
                if (ttl_ms < 0) ttl_ms = 0;
            }
            else if (a.size() != 3) {
                // any other arity like SET k v EX (missing number)
                return resp_error("syntax error");
            }

            auto& sh = store_.shard_for(key);
            sh.set(key, val);
            if (ttl_ms >= 0) {
                auto tp = std::chrono::steady_clock::now() + std::chrono::milliseconds(ttl_ms);
                sh.set_expire(key, tp);
            }
            return resp_simple("OK");
            };

        // PEXPIRE key ms
        h_["PEXPIRE"] = [this](auto const& a) {
            if (a.size() < 3) return resp_error("wrong #args for 'pexpire'");
            const std::string& key = a[1];
            long long ms = 0;
            try { ms = std::stoll(a[2]); }
            catch (...) { return resp_error("value is not an integer or out of range"); }
            if (ms < 0) ms = 0;
            auto& sh = store_.shard_for(key);
            auto cur = sh.get(key);      // lazily evicts if expired
            if (!cur) return resp_int(0);
            sh.set_expire(key, std::chrono::steady_clock::now() + std::chrono::milliseconds(ms));
            return resp_int(1);
            };

        // PERSIST key (remove TTL)
        h_["PERSIST"] = [this](auto const& a) {
            if (a.size() < 2) return resp_error("wrong #args for 'persist'");
            const std::string& key = a[1];
            auto& sh = store_.shard_for(key);

            // Check existence (also lazily evicts if expired)
            auto v = sh.get(key);
            if (!v) return resp_int(0);

            // If key exists, just erase TTL metadata
            sh.clear_expire(key);
            return resp_int(1);
            };

        h_["EXISTS"] = [this](auto const& a) {
            if (a.size() < 2) return resp_error("wrong #args for 'exists'");
            long long count = 0;
            auto now = std::chrono::steady_clock::now();
            for (size_t i = 1; i < a.size(); ++i) {
                const std::string& key = a[i];
                auto& sh = store_.shard_for(key);
                auto t = sh.type_of(key, now);
                if (t != ValueType::None) ++count;
            }
            return resp_int(count);
            };

        // HSET key field value
        h_["HSET"] = [this](auto const& a) {
            if (a.size() < 4 || ((a.size() - 2) % 2 != 0))
                return resp_error("wrong #args for 'hset'");

            const std::string& key = a[1];
            auto& sh = store_.shard_for(key);

            auto now = std::chrono::steady_clock::now();
            auto t = sh.type_of(key, now);
            if (t == ValueType::String) return resp_wrongtype();

            int added = 0;
            for (size_t i = 2; i + 1 < a.size(); i += 2) {
                const std::string& field = a[i];
                const std::string& value = a[i + 1];
                added += sh.hset(key, field, value); // 1 if new field, 0 if updated
            }
            return resp_int(added);
            };

        // HGET key field
        h_["HGET"] = [this](auto const& a) {
            if (a.size() < 3) return resp_error("wrong #args for 'hget'");
            const std::string& key = a[1];
            const std::string& field = a[2];

            auto& sh = store_.shard_for(key);
            auto now = std::chrono::steady_clock::now();
            auto t = sh.type_of(key, now);
            if (t == ValueType::String) return resp_wrongtype();

            auto v = sh.hget(key, field);
            if (!v) return resp_nil();
            return resp_bulk(*v);
            };

        // HDEL key field
        h_["HDEL"] = [this](auto const& a) {
            if (a.size() < 3) return resp_error("wrong #args for 'hdel'");
            const std::string& key = a[1];
            const std::string& field = a[2];

            auto& sh = store_.shard_for(key);
            auto now = std::chrono::steady_clock::now();
            auto t = sh.type_of(key, now);
            if (t == ValueType::String) return resp_wrongtype();

            int removed = sh.hdel(key, field);
            return resp_int(removed);
            };

        // HEXISTS key field
        h_["HEXISTS"] = [this](auto const& a) {
            if (a.size() < 3) return resp_error("wrong #args for 'hexists'");
            const std::string& key = a[1];
            const std::string& field = a[2];

            auto& sh = store_.shard_for(key);
            auto now = std::chrono::steady_clock::now();
            auto t = sh.type_of(key, now);
            if (t == ValueType::String) return resp_wrongtype();

            int ex = sh.hexists(key, field);
            return resp_int(ex);
            };

        // HLEN key
        h_["HLEN"] = [this](auto const& a) {
            if (a.size() < 2) return resp_error("wrong #args for 'hlen'");
            const std::string& key = a[1];

            auto& sh = store_.shard_for(key);
            auto now = std::chrono::steady_clock::now();
            auto t = sh.type_of(key, now);
            if (t == ValueType::String) return resp_wrongtype();

            long long n = sh.hlen(key);
            return resp_int(n);
            };

        // HGETALL key  -> array: [field, value, field, value, ...]
        h_["HGETALL"] = [this](auto const& a) {
            if (a.size() < 2) return resp_error("wrong #args for 'hgetall'");
            const std::string& key = a[1];

            auto& sh = store_.shard_for(key);
            auto now = std::chrono::steady_clock::now();
            auto t = sh.type_of(key, now);
            if (t == ValueType::String) return resp_wrongtype();

            auto vec = sh.hgetall(key);
            return resp_array(vec, /*as_bulk=*/true);
            };

        h_["TYPE"] = [this](auto const& a) {
            if (a.size() < 2) return resp_error("wrong #args for 'type'");
            const std::string& key = a[1];
            auto& sh = store_.shard_for(key);
            auto now = std::chrono::steady_clock::now();
            switch (sh.type_of(key, now)) {
            case ValueType::None:   return resp_bulk("none");
            case ValueType::String: return resp_bulk("string");
            case ValueType::Hash:   return resp_bulk("hash");
            }
            return resp_bulk("none");
            };

        h_["MGET"] = [this](auto const& a) {
            if (a.size() < 2) return resp_error("wrong #args for 'mget'");
            auto now = std::chrono::steady_clock::now();

            // type check first
            for (size_t i = 1; i < a.size(); ++i) {
                auto& sh = store_.shard_for(a[i]);
                auto t = sh.type_of(a[i], now);
                if (t == ValueType::Hash) return resp_wrongtype(); // <-- std::string
            }

            std::ostringstream arr;
            arr << "*" << (a.size() - 1) << "\r\n";
            for (size_t i = 1; i < a.size(); ++i) {
                auto& sh = store_.shard_for(a[i]);
                auto v = sh.get(a[i]);
                if (!v) { arr << "$-1\r\n"; }
                else { arr << "$" << v->size() << "\r\n" << *v << "\r\n"; }
            }
            return arr.str(); // std::string
            };


        h_["HMGET"] = [this](auto const& a) {
            if (a.size() < 3) return resp_error("wrong #args for 'hmget'");
            const std::string& key = a[1];
            auto& sh = store_.shard_for(key);
            auto now = std::chrono::steady_clock::now();
            auto t = sh.type_of(key, now);
            if (t == ValueType::String) return resp_wrongtype(); // <-- std::string

            std::ostringstream arr;
            arr << "*" << (a.size() - 2) << "\r\n";
            for (size_t i = 2; i < a.size(); ++i) {
                auto v = sh.hget(key, a[i]);
                if (!v) { arr << "$-1\r\n"; }
                else { arr << "$" << v->size() << "\r\n" << *v << "\r\n"; }
            }
            return arr.str();
            };


        h_["MSET"] = [this](auto const& a) {
            if ((a.size() < 3) || ((a.size() - 1) % 2 != 0)) return resp_error("wrong #args for 'mset'");
            for (size_t i = 1; i + 1 < a.size(); i += 2) {
                const std::string& key = a[i];
                const std::string& val = a[i + 1];
                store_.shard_for(key).set(key, val);
            }
            return resp_simple("OK");
            };

    }

    std::string Router::dispatch(const std::vector<std::string>& args) {
        if (args.empty()) return resp_error("empty");
        auto cmd = upper(args[0]);
        auto it = h_.find(cmd);
        if (it == h_.end()) return resp_error("unknown command");
        return it->second(args);
    }

} // namespace redisx

