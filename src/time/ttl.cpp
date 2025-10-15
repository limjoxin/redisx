#include <redisx/time/ttl.hpp>

#include <queue>
#include <unordered_map>
#include <utility>
#include <memory>
#include <vector>
#include <functional>
#include <string>
#include <optional>
#include <tuple>

namespace redisx::ttl {

    // Heap node: (when, key, generation)
    using HeapNode = std::tuple<TimePt, std::string, std::uint64_t>;

    struct Index::Impl {
        // Single-threaded per shard → no locking
        std::priority_queue<HeapNode, std::vector<HeapNode>, std::greater<HeapNode>> heap;
        std::unordered_map<std::string, std::uint64_t> gens;   // current gen per key
        std::unordered_map<std::string, TimePt> expires;       // current expiry per key
        std::uint64_t next_gen = 1;                            // monotonically increasing
    };

    Index::Index() : impl_(new Impl) {}
    Index::~Index() { delete impl_; }
    Index::Index(Index&& o) noexcept : impl_(std::exchange(o.impl_, nullptr)) {}
    Index& Index::operator=(Index&& o) noexcept {
        if (this != &o) { delete impl_; impl_ = std::exchange(o.impl_, nullptr); }
        return *this;
    }

    // Set/update expiry for key
    void Index::set(const std::string& key, TimePt when) {
        auto& I = *impl_;
        auto& g = I.gens[key];          // default 0 if new
        g = ++I.next_gen;               // bump to a fresh generation
        I.expires[key] = when;
        I.heap.emplace(when, key, g);   // old nodes for this key become stale
    }

    // Clear expiry for key (PERSIST / SET without TTL)
    void Index::clear(const std::string& key) {
        auto& I = *impl_;
        auto it = I.gens.find(key);
        if (it != I.gens.end()) {
            it->second = ++I.next_gen;  // bump so old heap nodes turn stale
        }
        I.expires.erase(key);
    }

    // Next wake-up time if heap not empty
    std::optional<TimePt> Index::next_due() const {
        const auto& I = *impl_;
        if (I.heap.empty()) return std::nullopt;
        return std::get<0>(I.heap.top());
    }

    // Drop stale nodes sitting at the top (cheap, rarely needed)
    // sweep_due naturally ignores stales, so this is optional hygiene.
    void Index::prune() {
        auto& I = *impl_;
        while (!I.heap.empty()) {
            const auto& n = I.heap.top();
            const auto& key = std::get<1>(n);
            const auto  gen = std::get<2>(n);
            auto it = I.gens.find(key);
            if (it == I.gens.end() || it->second != gen) {
                I.heap.pop(); // stale
                continue;
            }
            break;
        }
    }

    // ---- Internal helper used by sweep_due -------------------------------------

    void Index::pop_due_internal(TimePt now,
        bool& have_due,
        bool& valid,
        std::string& out_key)
    {
        auto& I = *impl_;
        have_due = false;
        valid = false;
        out_key.clear();

        // Drop stale nodes at the top
        while (!I.heap.empty()) {
            const auto& n = I.heap.top();
            const auto& key = std::get<1>(n);
            const auto  gen = std::get<2>(n);
            auto itg = I.gens.find(key);
            if (itg == I.gens.end() || itg->second != gen) {
                I.heap.pop();   // stale node
                continue;
            }
            break;              // top is current for that key
        }

        if (I.heap.empty()) return;

        // Is the current top due?
        const auto& n = I.heap.top();
        const auto& when = std::get<0>(n);
        const auto& key = std::get<1>(n);

        if (when <= now) {
            I.heap.pop();
            // Double-check expiry (may have been cleared/updated)
            auto it = I.expires.find(key);
            if (it != I.expires.end() && it->second == when) {
                have_due = true;
                valid = true;
                out_key = key;
                // Caller (the shard) should check its real expire_at and erase if expired
            }
            else {
                // Consumed something (progress), but it was stale; ask caller to loop again.
                have_due = true;
                valid = false;
            }
        }
        else {
            have_due = false; // nothing due yet
        }
    }

} // namespace redisx::ttl

