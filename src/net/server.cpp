#include <redisx/net/server.hpp>
#include <redisx/net/session.hpp>
using asio::ip::tcp;

namespace redisx {

    Server::Server(asio::io_context& io, uint16_t port, Router& router, ThreadPool& pool)
        : acceptor_(io, tcp::endpoint(tcp::v4(), port))
        , router_(router)
        , pool_(pool) {
        accept();
    }

    void Server::accept() {
        acceptor_.async_accept([this](std::error_code ec, tcp::socket socket) {
            if (!ec) {
                std::make_shared<Session>(std::move(socket), router_, pool_)->start();
            }
            accept();
            });
    }

} // namespace redisx

