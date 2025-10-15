#pragma once
#include <string>

namespace redisx {

	class Snapshot {
	public:
		bool save(const std::string&) { return true; }
		bool load(const std::string&) { return true; }
	};

} // namespace redisx
