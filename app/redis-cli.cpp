#include <asio.hpp>
#include <iostream>
#include <string>
#include <vector>
#include <optional>
#include <sstream>
#include <cctype>

using asio::ip::tcp;

// ---------- simple tokenizer: splits like a shell (supports "quoted strings")
static std::vector<std::string> tokenize(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    bool inq = false;
    char q = 0;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (!inq && std::isspace(static_cast<unsigned char>(c))) {
            if (!cur.empty()) { out.push_back(cur); cur.clear(); }
            continue;
        }
        if (!inq && (c == '"' || c == '\'')) { inq = true; q = c; continue; }
        if (inq && c == q) { inq = false; continue; }
        if (inq && c == '\\' && i + 1 < line.size()) {
            char n = line[++i];
            switch (n) {
            case 'n': cur.push_back('\n'); break;
            case 'r': cur.push_back('\r'); break;
            case 't': cur.push_back('\t'); break;
            case '"': cur.push_back('"'); break;
            case '\'': cur.push_back('\''); break;
            case '\\': cur.push_back('\\'); break;
            default: cur.push_back(n); break;
            }
            continue;
        }
        cur.push_back(c);
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// ---------- build RESP Array of Bulk Strings
static std::string to_resp(const std::vector<std::string>& args) {
    std::ostringstream o;
    o << "*" << args.size() << "\r\n";
    for (auto& a : args) {
        o << "$" << a.size() << "\r\n" << a << "\r\n";
    }
    return o.str();
}

// ---------- RESP reader & pretty-printer
struct RespVal {
    enum class Type { Simple, Error, Int, Bulk, Nil, Array };
    Type type = Type::Nil;
    std::string s;                // Simple/Error/Bulk
    long long i = 0;              // Int
    std::vector<RespVal> arr;     // Array
};

static bool read_line(tcp::socket& sock, std::string& buf, std::string& line) {
    for (;;) {
        auto pos = buf.find("\r\n");
        if (pos != std::string::npos) {
            line.assign(buf.data(), pos);
            buf.erase(0, pos + 2);
            return true;
        }
        char tmp[4096];
        asio::error_code ec;
        std::size_t n = sock.read_some(asio::buffer(tmp), ec);
        if (ec) return false;
        buf.append(tmp, tmp + n);
    }
}

static bool read_exact(tcp::socket& sock, std::string& buf, std::string& out, std::size_t nbytes) {
    while (buf.size() < nbytes) {
        char tmp[4096];
        asio::error_code ec;
        std::size_t n = sock.read_some(asio::buffer(tmp), ec);
        if (ec) return false;
        buf.append(tmp, tmp + n);
    }
    out.assign(buf.data(), nbytes);
    buf.erase(0, nbytes);
    return true;
}

static bool parse_resp_value(tcp::socket& sock, std::string& buf, RespVal& out);

static bool parse_bulk(tcp::socket& sock, std::string& buf, RespVal& out) {
    std::string line;
    if (!read_line(sock, buf, line)) return false;
    long long len = 0;
    try { len = std::stoll(line); }
    catch (...) { return false; }
    if (len == -1) { out.type = RespVal::Type::Nil; return true; }
    if (len < 0) return false;
    std::string data;
    if (!read_exact(sock, buf, data, static_cast<std::size_t>(len))) return false;
    std::string dummy;
    if (!read_line(sock, buf, dummy)) return false;
    out.type = RespVal::Type::Bulk;
    out.s = std::move(data);
    return true;
}

static bool parse_array(tcp::socket& sock, std::string& buf, RespVal& out) {
    std::string line;
    if (!read_line(sock, buf, line)) return false;
    long long n = 0;
    try { n = std::stoll(line); }
    catch (...) { return false; }
    if (n == -1) { out.type = RespVal::Type::Nil; return true; }
    if (n < 0) return false;
    out.type = RespVal::Type::Array;
    out.arr.reserve(static_cast<std::size_t>(n));
    for (long long i = 0; i < n; ++i) {
        RespVal v;
        if (!parse_resp_value(sock, buf, v)) return false;
        out.arr.push_back(std::move(v));
    }
    return true;
}

static bool parse_resp_value(tcp::socket& sock, std::string& buf, RespVal& out) {
    while (buf.empty()) {
        char tmp[4096];
        asio::error_code ec;
        std::size_t n = sock.read_some(asio::buffer(tmp), ec);
        if (ec) return false;
        buf.append(tmp, tmp + n);
    }
    char t = buf[0]; buf.erase(0, 1);
    switch (t) {
    case '+': {
        std::string line; if (!read_line(sock, buf, line)) return false;
        out.type = RespVal::Type::Simple; out.s = std::move(line); return true;
    }
    case '-': {
        std::string line; if (!read_line(sock, buf, line)) return false;
        out.type = RespVal::Type::Error;  out.s = std::move(line); return true;
    }
    case ':': {
        std::string line; if (!read_line(sock, buf, line)) return false;
        try { out.i = std::stoll(line); }
        catch (...) { return false; }
        out.type = RespVal::Type::Int; return true;
    }
    case '$': return parse_bulk(sock, buf, out);
    case '*': return parse_array(sock, buf, out);
    default:  return false;
    }
}

static void print_val(const RespVal& v) {
    switch (v.type) {
    case RespVal::Type::Simple: std::cout << v.s << "\n"; break;
    case RespVal::Type::Error:  std::cout << "(error) " << v.s << "\n"; break;
    case RespVal::Type::Int:    std::cout << "(integer) " << v.i << "\n"; break;
    case RespVal::Type::Bulk:   std::cout << (v.s.empty() ? "\"\"" : "\"" + v.s + "\"") << "\n"; break;
    case RespVal::Type::Nil:    std::cout << "(nil)\n"; break;
    case RespVal::Type::Array:
        if (v.arr.empty()) { std::cout << "(empty array)\n"; break; }
        for (size_t i = 0; i < v.arr.size(); ++i) {
            std::cout << i + 1 << ") ";
            if (v.arr[i].type == RespVal::Type::Array) {
                std::cout << "\n";
                print_val(v.arr[i]);
            }
            else {
                print_val(v.arr[i]);
            }
        }
        break;
    }
}

int main(int argc, char** argv) {
    std::string host = "127.0.0.1";
    uint16_t port = 6379;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if ((a == "-h" || a == "--host") && i + 1 < argc) { host = argv[++i]; }
        else if ((a == "-p" || a == "--port") && i + 1 < argc) { port = static_cast<uint16_t>(std::stoi(argv[++i])); }
        else if (a == "-?" || a == "--help") {
            std::cout << "Usage: redis-cli [-h host] [-p port]\n"; return 0;
        }
    }

    try {
        asio::io_context io;
        tcp::resolver res(io);
        auto eps = res.resolve(host, std::to_string(port));
        tcp::socket sock(io);
        asio::connect(sock, eps);

        std::cout << "Connected to " << host << ":" << port << "\n";
        std::cout << "Type commands like:  PING  |  SET a \"hello\"  |  GET a  |  EXPIRE a 2\n";
        std::cout << "Ctrl+C to quit.\n";
        std::string readbuf;

        for (;;) {
            std::cout << "> ";
            std::string line;
            if (!std::getline(std::cin, line)) break;
            auto args = tokenize(line);
            if (args.empty()) continue;
            if (args.size() == 1 && (args[0] == "QUIT" || args[0] == "quit")) break;

            std::string req = to_resp(args);
            asio::write(sock, asio::buffer(req));

            RespVal reply;
            if (!parse_resp_value(sock, readbuf, reply)) {
                std::cout << "(protocol/read error)\n"; break;
            }
            print_val(reply);
        }

        sock.close();
    }
    catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
