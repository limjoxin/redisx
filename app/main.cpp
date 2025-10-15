#include <asio.hpp>
#include <iostream>
#include <chrono>
#include <thread>                      // <-- add this
#include <algorithm>
#include <redisx/util/thread_pool.hpp>
#include <redisx/core/store.hpp>
#include <redisx/core/router.hpp>
#include <redisx/net/server.hpp>

using namespace redisx;

int main(int argc, char** argv) {
    uint16_t port = 6379;
    size_t shards = 0; // 0 => auto = hardware_concurrency()

    // simple arg parsing: --port/-p, --shards
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if ((a == "--port" || a == "-p") && i + 1 < argc) {
            port = static_cast<uint16_t>(std::stoi(argv[++i]));
        }
        else if (a == "--shards" && i + 1 < argc) {
            shards = static_cast<size_t>(std::stoull(argv[++i]));
        }
        else if (a == "--help" || a == "-?") {
            std::cout << "Usage: redisx-server [--port N] [--shards N]\n";
            return 0;
        }
        else if (i == 1 && a.find_first_not_of("0123456789") == std::string::npos) {
            // backward-compat: first arg as port (e.g., "6379")
            port = static_cast<uint16_t>(std::stoi(a));
        }
    }

    if (shards == 0) {
        unsigned hc = std::thread::hardware_concurrency();
        if (hc == 0) hc = 4;
        shards = hc;
    }

    asio::io_context io;

    // keep one thread for Asio, rest for workers
    unsigned hc = std::max(1u, std::thread::hardware_concurrency());
    ThreadPool pool(std::max(1u, hc > 1 ? hc - 1 : 1));

    Store store(shards);
    Router router(store);
    Server server(io, port, router, pool);

    // TTL sweep timer
    asio::steady_timer timer{ io };
    auto arm = [&](auto&& self) -> void {
        timer.expires_after(std::chrono::milliseconds(200));
        timer.async_wait([&](const asio::error_code& ec) {
            if (ec) return;
            store.sweep_all();
            self(self);
            });
        };
    arm(arm);

    std::cout << "redisx RESP server on " << port
        << " with " << shards << " shard" << (shards == 1 ? "" : "s") << " ...\n";

    io.run();
    return 0;
}

