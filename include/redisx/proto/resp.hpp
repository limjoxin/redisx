#pragma once
#include <string>
#include <vector>
#include <optional>
#include <cstddef>

namespace redisx {

	// A full RESP2 frame parsed as an array of bulk strings,
	// which matches how Redis commands are typically sent.
	struct RespArray { std::vector<std::string> args; };

	// Result of trying to parse from a byte buffer.
	struct RespParseResult {
		std::optional<RespArray> arr;  // present when a full frame was parsed
		std::string error;             // non-empty on protocol error
		std::size_t consumed = 0;      // how many bytes to erase from the buffer
	};

	// Parse exactly one RESP2 array frame from [data, data+len].
	// If incomplete, returns {arr=nullopt, error="", consumed=0}.
	// If protocol error, returns {arr=nullopt, error="...", consumed=bytes_to_drop_or_0}.
	RespParseResult parse_resp(const char* data, std::size_t len);

	// Emit helpers
	std::string resp_simple(const std::string& s);  // +OK\r\n
	std::string resp_error(const std::string& s);   // -ERR msg\r\n
	std::string resp_bulk(const std::string& s);    // $len\r\n...\r\n
	std::string resp_nil();                         // $-1\r\n
	std::string resp_int(long long v);              // :n\r\n

	// Array emitter (useful for e.g. MGET later)
	std::string resp_array(const std::vector<std::string>& items, bool as_bulk = true);

} // namespace redisx
