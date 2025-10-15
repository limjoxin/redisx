#include <redisx/proto/resp.hpp>
#include <sstream>
#include <string_view>
#include <limits>
#include <cstdlib>

namespace redisx {

    // ---------- helpers
    static inline long long parse_ll(std::string_view s, bool& ok) {
        ok = true;
        if (s.empty()) { ok = false; return 0; }
        // no locale, simple parse
        long long sign = 1; size_t i = 0;
        if (s[0] == '-') { sign = -1; i = 1; }
        long long v = 0;
        for (; i < s.size(); ++i) {
            char c = s[i];
            if (c < '0' || c > '9') { ok = false; return 0; }
            long long d = (c - '0');
            if (v > (std::numeric_limits<long long>::max() - d) / 10) { ok = false; return 0; }
            v = v * 10 + d;
        }
        return v * sign;
    }

    static inline bool read_line(const char* data, std::size_t len, std::size_t off, std::size_t& line_end) {
        // find "\r\n" starting at off
        for (std::size_t i = off; i + 1 < len; ++i) {
            if (data[i] == '\r' && data[i + 1] == '\n') { line_end = i; return true; }
        }
        return false;
    }

    // Reads a RESP "line" (without the trailing CRLF). Returns {ok, start, end, next_offset}
    // where [start,end) is the line content, and next_offset points after CRLF.
    struct Line {
        bool ok = false;
        std::size_t start = 0, end = 0, next = 0;
    };
    static inline Line get_line(const char* data, std::size_t len, std::size_t off) {
        Line L;
        std::size_t e;
        if (!read_line(data, len, off, e)) return L;
        L.ok = true; L.start = off; L.end = e; L.next = e + 2; // skip CRLF
        return L;
    }

    // main parser
    RespParseResult parse_resp(const char* data, std::size_t len) {
        RespParseResult r;
        if (len == 0) return r;

        // expect an Array: "*<n>\r\n" then n Bulk strings "$<len>\r\n<data>\r\n"
        if (data[0] != '*') {
            // reject inline command
            r.error = "protocol error: expected array";
            r.consumed = 0;
            return r;
        }

        // read "*<n>\r\n"
        auto L = get_line(data, len, 1);
        if (!L.ok) return r; // need more
        bool ok = false;
        long long n = parse_ll(std::string_view{ data + 1, L.end - 1 }, ok);
        if (!ok || n < 0) { r.error = "protocol error: bad array length"; r.consumed = L.next; return r; }
        std::size_t off = L.next;

        RespArray arr;
        arr.args.reserve(static_cast<std::size_t>(n));
        for (long long i = 0; i < n; ++i) {
            if (off >= len) return r; // need more
            if (data[off] != '$') { r.error = "protocol error: expected bulk string"; r.consumed = off; return r; }
            // "$<len>\r\n"
            auto L2 = get_line(data, len, off + 1);
            if (!L2.ok) return r; // need more
            long long blen = parse_ll(std::string_view{ data + off + 1, L2.end - (off + 1) }, ok);
            if (!ok) { r.error = "protocol error: bad bulk length"; r.consumed = L2.next; return r; }
            off = L2.next;
            if (blen == -1) {
                // Null bulk -> treat as empty arg
                arr.args.emplace_back();
                continue;
            }
            if (blen < 0) { r.error = "protocol error: negative bulk length"; r.consumed = off; return r; }
            // Need blen bytes + "\r\n"
            if (off + static_cast<std::size_t>(blen) + 2 > len) return r; // need more
            arr.args.emplace_back(data + off, data + off + blen);
            off += static_cast<std::size_t>(blen);
            // expect CRLF after bulk
            if (off + 1 >= len || data[off] != '\r' || data[off + 1] != '\n') { r.error = "protocol error: bulk missing CRLF"; r.consumed = off; return r; }
            off += 2;
        }

        r.arr = std::move(arr);
        r.consumed = off;
        return r;
    }

    // emitters
    std::string resp_simple(const std::string& s) { return "+" + s + "\r\n"; }
    std::string resp_error(const std::string& s) { return "-ERR " + s + "\r\n"; }

    std::string resp_bulk(const std::string& s) {
        std::ostringstream o; o << "$" << s.size() << "\r\n" << s << "\r\n"; return o.str();
    }

    std::string resp_nil() { return "$-1\r\n"; }

    std::string resp_int(long long v) {
        std::ostringstream o; o << ":" << v << "\r\n"; return o.str();
    }

    std::string resp_array(const std::vector<std::string>& items, bool as_bulk) {
        std::ostringstream o; o << "*" << items.size() << "\r\n";
        for (auto& it : items) {
            if (as_bulk) o << "$" << it.size() << "\r\n" << it << "\r\n";
            else         o << it; // (not used now)
        }
        return o.str();
    }

} // namespace redisx

