#pragma once
#include <string>
#include <vector>
#include <utility>

namespace redisx {

	// placeholder; fill in later
	class ZSet {
	public:
		bool add(double, const std::string&) { return false; }
		bool remove(const std::string&) { return false; }
		bool score_of(const std::string&, double&) const { return false; }
		std::vector<std::pair<std::string, double>> range(long, long) const { return {}; }
	};

} // namespace redisx
