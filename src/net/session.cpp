#include <redisx/net/session.hpp>
#include <asio/bind_executor.hpp>
#include <asio/write.hpp>

using asio::ip::tcp;

namespace redisx {

    Session::Session(tcp::socket sock, Router& router, ThreadPool& pool)
        : socket_(std::move(sock))
        , ex_(socket_.get_executor())
        , strand_(ex_)
        , router_(router)
        , pool_(pool) {
        inbuf_.resize(8 * 1024);
    }

    void Session::start() { do_read(); }

    void Session::do_read() {
        auto self = shared_from_this();
        socket_.async_read_some(asio::buffer(inbuf_),
            [this, self](std::error_code ec, std::size_t n) {
                if (ec) return;
                pending_.append(inbuf_.data(), n);

                for (;;) {
                    auto res = parse_resp(pending_.data(), pending_.size());
                    if (!res.arr && res.error.empty()) break;       // need more
                    if (!res.error.empty()) {
                        asio::post(strand_, [self] { self->enqueue_write("-ERR proto\r\n"); });
                        pending_.erase(0, res.consumed);
                        continue;
                    }
                    auto args = std::move(res.arr->args);
                    pending_.erase(0, res.consumed);
                    handle_frame(args);
                }
                do_read();
            });
    }

    void Session::handle_frame(const std::vector<std::string>& args) {
        auto self = shared_from_this();
        pool_.enqueue([this, self, args] {
            std::string reply;
            try {
                reply = router_.dispatch(args);
            }
            catch (const std::exception& e) {
                reply = resp_error(std::string("server error: ") + e.what());
            }
            catch (...) {
                reply = resp_error("server error");
            }
            asio::post(strand_, [self, r = std::move(reply)]() mutable {
                self->enqueue_write(std::move(r));
                });
            });
    }

    void Session::enqueue_write(std::string msg) {
        bool writing = !outq_.empty();
        outq_.push_back(std::move(msg));
        if (!writing) do_write();
    }

    void Session::do_write() {
        auto self = shared_from_this();
        asio::async_write(socket_, asio::buffer(outq_.front()),
            asio::bind_executor(strand_,
                [this, self](std::error_code ec, std::size_t) {
                    if (ec) return;
                    outq_.pop_front();
                    if (!outq_.empty()) do_write();
                }));
    }

} // namespace redisx
