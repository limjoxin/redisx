#pragma once
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>
#include <redisx/core/store.hpp>

namespace redisx {

	class Router {
	public:
		using Handler = std::function<std::string(const std::vector<std::string>&)>;
		explicit Router(Store& s);
		std::string dispatch(const std::vector<std::string>& args);

	private:
		Store& store_;
		std::unordered_map<std::string, Handler> h_;
	};

} // namespace redisx
