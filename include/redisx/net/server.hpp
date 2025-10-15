#pragma once
#include <asio.hpp>
#include <redisx/core/router.hpp>
#include <redisx/util/thread_pool.hpp>

namespace redisx {

	class Server {
	public:
		Server(asio::io_context& io, uint16_t port, Router& router, ThreadPool& pool);
	private:
		void accept();
		asio::ip::tcp::acceptor acceptor_;
		Router& router_;
		ThreadPool& pool_;
	};

} // namespace redisx
