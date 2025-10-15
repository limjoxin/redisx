#pragma once
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace redisx::ttl {

    // ---- clocks & common types -------------------------------------------------

    using Clock = std::chrono::steady_clock;    // monotonic
    using TimePt = Clock::time_point;
    using Ms = std::chrono::milliseconds;

    // Redis TTL semantics for reference by callers
    constexpr long long TTL_NO_KEY = -2;   // key absent (caller decides)
    constexpr long long TTL_NO_TTL = -1;   // key present, no expiry

    // Now (monotonic)
    inline TimePt now() { return Clock::now(); }

    // Build absolute expiries from relative durations
    inline TimePt from_seconds(long long sec, TimePt base = now()) {
        if (sec < 0) sec = 0;
        return base + std::chrono::seconds(sec);
    }
    inline TimePt from_milliseconds(long long ms, TimePt base = now()) {
        if (ms < 0) ms = 0;
        return base + Ms(ms);
    }

    // Check expiry and compute remaining
    inline bool is_expired(std::optional<TimePt> when, TimePt t = now()) {
        return when && t >= *when;
    }
    inline long long remaining_ms(std::optional<TimePt> when, TimePt t = now()) {
        if (!when) return TTL_NO_TTL;
        auto diff = std::chrono::duration_cast<Ms>(*when - t).count();
        return diff < 0 ? 0 : diff;
    }

    // ---- Per-shard expiry index (PIMPL; single-threaded owner) -----------------

    class Index {
    public:
        Index();
        ~Index();
        Index(const Index&) = delete;
        Index& operator=(const Index&) = delete;
        Index(Index&&) noexcept;
        Index& operator=(Index&&) noexcept;

        // record/update expiry for key
        void set(const std::string& key, TimePt when);

        // remove expiry tracking for key (persist)
        void clear(const std::string& key);

        // Pop all due entries (<= now) and call on_expire(key).
        // The callback should erase the key if it is indeed expired.
        template <class Fn>
        void sweep_due(TimePt at, Fn&& on_expire) {
            while (true) {
                std::string key;
                bool have_due = false;
                bool valid = false;
                pop_due_internal(at, have_due, valid, key);
                if (!have_due) break;   // nothing due right now
                if (!valid) continue;   // consumed stale node; keep looping
                on_expire(key);         // shard decides final erase after checking actual expire_at
            }
        }

        // Next wake-up time if any key is scheduled; std::nullopt if empty
        std::optional<TimePt> next_due() const;

        // Maintenance: drop heap junk (rare; mostly for tests)
        void prune();

    private:
        struct Impl;           // opaque implementation
        Impl* impl_{ nullptr };

        // Examine heap top, drop stales, and indicate if a due/current node exists.
        // If a current & due node exists, it is popped and (have_due=true, valid=true, out_key set).
        // If we only popped stale nodes, (have_due=true, valid=false) to let caller loop again.
        void pop_due_internal(TimePt now,
            bool& have_due,
            bool& valid,
            std::string& out_key);
    };

} // namespace redisx::ttl

