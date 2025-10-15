#pragma once
#include <asio.hpp>
#include <deque>
#include <memory>
#include <string>
#include <vector>
#include <redisx/core/router.hpp>
#include <redisx/util/thread_pool.hpp>
#include <redisx/proto/resp.hpp>

namespace redisx {

	class Session : public std::enable_shared_from_this<Session> {
	public:
		Session(asio::ip::tcp::socket sock, Router& router, ThreadPool& pool);
		void start();

	private:
		void do_read();
		void do_write();
		void enqueue_write(std::string msg);
		void handle_frame(const std::vector<std::string>& args);

		asio::ip::tcp::socket socket_;
		asio::any_io_executor ex_;
		asio::strand<asio::any_io_executor> strand_;
		std::vector<char> inbuf_;
		std::string pending_;
		std::deque<std::string> outq_;

		Router& router_;
		ThreadPool& pool_;
	};

} // namespace redisx
