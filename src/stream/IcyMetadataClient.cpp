#include "pcmtp/stream/IcyMetadataClient.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <openssl/ssl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <cstring>
#include <algorithm>
#include <mutex>
#include <sstream>
#include <vector>

#include "pcmtp/util/Logger.hpp"

namespace pcmtp {
namespace {

struct HttpEndpoint {
    std::string scheme = "http";
    std::string host;
    std::string path;
    int port = 80;
};

bool starts_with_ci(const std::string& text, const char* prefix) {
    for (std::size_t i = 0; prefix[i] != '\0'; ++i) {
        if (i >= text.size()) {
            return false;
        }
        if (std::tolower(static_cast<unsigned char>(text[i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i]))) {
            return false;
        }
    }
    return true;
}

bool parse_http_endpoint(const std::string& url, HttpEndpoint& out) {
    std::string normalized = url;
    if (starts_with_ci(normalized, "icy://")) {
        normalized = "http://" + normalized.substr(6);
    }

    std::size_t scheme_end = normalized.find("://");
    if (scheme_end == std::string::npos) {
        return false;
    }

    out.scheme = normalized.substr(0, scheme_end);
    std::transform(out.scheme.begin(), out.scheme.end(), out.scheme.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (out.scheme != "http" && out.scheme != "https") {
        return false;
    }

    const std::size_t start = scheme_end + 3;
    std::size_t slash = normalized.find('/', start);
    const std::string authority = slash == std::string::npos
        ? normalized.substr(start)
        : normalized.substr(start, slash - start);
    out.path = slash == std::string::npos ? "/" : normalized.substr(slash);
    if (out.path.empty()) {
        out.path = "/";
    }

    const std::size_t colon = authority.find(':');
    if (colon == std::string::npos) {
        out.host = authority;
        out.port = out.scheme == "https" ? 443 : 80;
    } else {
        out.host = authority.substr(0, colon);
        try {
            out.port = std::stoi(authority.substr(colon + 1));
        } catch (...) {
            return false;
        }
    }
    return !out.host.empty() && out.port > 0;
}

std::string resolve_redirect_url(const std::string& base_url, const std::string& location) {
    if (location.empty()) {
        return std::string();
    }
    if (starts_with_ci(location, "http://") || starts_with_ci(location, "https://")) {
        return location;
    }

    HttpEndpoint base;
    if (!parse_http_endpoint(base_url, base)) {
        return std::string();
    }

    std::ostringstream url;
    url << base.scheme << "://" << base.host;
    const bool default_port = (base.scheme == "http" && base.port == 80) ||
                              (base.scheme == "https" && base.port == 443);
    if (!default_port) {
        url << ":" << base.port;
    }

    if (!location.empty() && location[0] == '/') {
        url << location;
        return url.str();
    }

    std::string path = base.path.empty() ? "/" : base.path;
    const std::size_t slash = path.rfind('/');
    if (slash == std::string::npos) {
        url << "/" << location;
    } else {
        url << path.substr(0, slash + 1) << location;
    }
    return url.str();
}

SSL_CTX* client_ssl_context() {
    static SSL_CTX* context = nullptr;
    static std::once_flag once;
    std::call_once(once, []() {
        context = SSL_CTX_new(TLS_client_method());
        if (context != nullptr) {
            SSL_CTX_set_verify(context, SSL_VERIFY_NONE, nullptr);
        }
    });
    return context;
}

class HttpConnection {
public:
    HttpConnection() = default;
    ~HttpConnection() { close(); }

    HttpConnection(const HttpConnection&) = delete;
    HttpConnection& operator=(const HttpConnection&) = delete;

    int socket_fd() const { return fd_; }

    bool connect(const HttpEndpoint& endpoint,
                 const std::atomic<bool>& stop_requested,
                 std::atomic<int>* active_socket) {
        close();
        if (stop_requested.load(std::memory_order_relaxed)) {
            return false;
        }

        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* result = nullptr;
        const std::string port = std::to_string(endpoint.port);
        if (getaddrinfo(endpoint.host.c_str(), port.c_str(), &hints, &result) != 0 || result == nullptr) {
            return false;
        }

        for (addrinfo* cursor = result; cursor != nullptr; cursor = cursor->ai_next) {
            if (stop_requested.load(std::memory_order_relaxed)) {
                break;
            }
            fd_ = ::socket(cursor->ai_family, cursor->ai_socktype, cursor->ai_protocol);
            if (fd_ < 0) {
                continue;
            }

            const int flags = fcntl(fd_, F_GETFL, 0);
            if (flags >= 0) {
                fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
            }

            const int connect_result = ::connect(fd_, cursor->ai_addr, cursor->ai_addrlen);
            if (connect_result == 0) {
                if (flags >= 0) {
                    fcntl(fd_, F_SETFL, flags);
                }
                break;
            }
            if (connect_result < 0 && errno != EINPROGRESS) {
                ::close(fd_);
                fd_ = -1;
                continue;
            }

            bool connected = false;
            for (int waited_ms = 0; waited_ms < 5000 && !stop_requested.load(std::memory_order_relaxed); waited_ms += 200) {
                pollfd pfd = {fd_, POLLOUT, 0};
                const int ready = ::poll(&pfd, 1, 200);
                if (ready < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    break;
                }
                if (ready == 0) {
                    continue;
                }

                int socket_error = 0;
                socklen_t error_length = sizeof(socket_error);
                if (getsockopt(fd_, SOL_SOCKET, SO_ERROR, &socket_error, &error_length) != 0 || socket_error != 0) {
                    break;
                }
                connected = true;
                break;
            }

            if (!connected || stop_requested.load(std::memory_order_relaxed)) {
                ::close(fd_);
                fd_ = -1;
                continue;
            }

            if (flags >= 0) {
                fcntl(fd_, F_SETFL, flags);
            }
            break;
        }
        freeaddrinfo(result);
        if (fd_ < 0) {
            return false;
        }

        timeval timeout{};
        timeout.tv_sec = 5;
        timeout.tv_usec = 0;
        setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

        if (endpoint.scheme == "https") {
            SSL_CTX* context = client_ssl_context();
            if (context == nullptr) {
                close();
                return false;
            }
            ssl_ = SSL_new(context);
            if (ssl_ == nullptr) {
                close();
                return false;
            }
            SSL_set_fd(ssl_, fd_);
            SSL_set_tlsext_host_name(ssl_, endpoint.host.c_str());
            if (SSL_connect(ssl_) != 1) {
                close();
                return false;
            }
        }

        if (active_socket != nullptr) {
            active_socket->store(fd_, std::memory_order_relaxed);
        }
        return true;
    }

    void close() {
        if (active_socket_ != nullptr && fd_ >= 0) {
            int expected = fd_;
            active_socket_->compare_exchange_strong(expected, -1, std::memory_order_relaxed);
        }
        active_socket_ = nullptr;

        if (ssl_ != nullptr) {
            SSL_shutdown(ssl_);
            SSL_free(ssl_);
            ssl_ = nullptr;
        }
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    void track_active_socket(std::atomic<int>* active_socket) {
        active_socket_ = active_socket;
    }

    bool send_all(const char* data, std::size_t size) {
        std::size_t sent = 0;
        while (sent < size) {
            const ssize_t chunk = write(data + sent, size - sent);
            if (chunk <= 0) {
                return false;
            }
            sent += static_cast<std::size_t>(chunk);
        }
        return true;
    }

    bool send_request(const HttpEndpoint& endpoint) {
        std::ostringstream request;
        request << "GET " << endpoint.path << " HTTP/1.1\r\n"
                << "Host: " << endpoint.host;
        const bool default_port = (endpoint.scheme == "http" && endpoint.port == 80) ||
                                  (endpoint.scheme == "https" && endpoint.port == 443);
        if (!default_port) {
            request << ":" << endpoint.port;
        }
        request << "\r\n"
                << "User-Agent: pcm-transport/0.9\r\n"
                << "Icy-MetaData: 1\r\n"
                << "Connection: close\r\n"
                << "\r\n";
        const std::string request_text = request.str();
        return send_all(request_text.c_str(), request_text.size());
    }

    bool read_headers(std::string& headers, const std::atomic<bool>& stop_requested) {
        headers.clear();
        char byte = '\0';
        while (!stop_requested.load(std::memory_order_relaxed)) {
            const ssize_t got = read(&byte, 1);
            if (got < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                    continue;
                }
                return false;
            }
            if (got == 0) {
                return false;
            }
            headers.push_back(byte);
            if (headers.size() >= 4 &&
                headers.compare(headers.size() - 4, 4, "\r\n\r\n") == 0) {
                return true;
            }
            if (headers.size() > 65536) {
                return false;
            }
        }
        return false;
    }

    bool read_byte(unsigned char& byte, const std::atomic<bool>& stop_requested) {
        while (!stop_requested.load(std::memory_order_relaxed)) {
            const ssize_t got = read(&byte, 1);
            if (got < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                    continue;
                }
                return false;
            }
            if (got == 0) {
                return false;
            }
            return true;
        }
        return false;
    }

    bool recv_some(std::string& buffer, std::size_t max_bytes, const std::atomic<bool>& stop_requested) {
        std::vector<char> chunk(1024);
        while (buffer.size() < max_bytes) {
            if (stop_requested.load(std::memory_order_relaxed)) {
                return false;
            }
            const std::size_t want = std::min(max_bytes - buffer.size(), chunk.size());
            const ssize_t got = read(chunk.data(), want);
            if (got < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                    continue;
                }
                return false;
            }
            if (got == 0) {
                return false;
            }
            buffer.append(chunk.data(), static_cast<std::size_t>(got));
        }
        return true;
    }

    bool discard_bytes(std::size_t bytes, const std::atomic<bool>& stop_requested) {
        std::vector<char> buffer(4096);
        std::size_t remaining = bytes;
        while (remaining > 0) {
            if (stop_requested.load(std::memory_order_relaxed)) {
                return false;
            }
            const std::size_t chunk = std::min(remaining, buffer.size());
            const ssize_t got = read(buffer.data(), chunk);
            if (got < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                    continue;
                }
                return false;
            }
            if (got == 0) {
                return false;
            }
            remaining -= static_cast<std::size_t>(got);
        }
        return true;
    }

private:
    ssize_t read(void* buffer, std::size_t size) {
        if (ssl_ != nullptr) {
            const int got = SSL_read(ssl_, buffer, static_cast<int>(size));
            if (got <= 0) {
                const int error = SSL_get_error(ssl_, got);
                if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
                    errno = EAGAIN;
                    return -1;
                }
                if (got == 0) {
                    return 0;
                }
                return -1;
            }
            return got;
        }
        return ::recv(fd_, buffer, size, 0);
    }

    ssize_t write(const void* buffer, std::size_t size) {
        if (ssl_ != nullptr) {
            const int sent = SSL_write(ssl_, buffer, static_cast<int>(size));
            if (sent <= 0) {
                const int error = SSL_get_error(ssl_, sent);
                if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
                    errno = EAGAIN;
                    return -1;
                }
                return -1;
            }
            return sent;
        }
        return ::send(fd_, buffer, size, 0);
    }

    int fd_ = -1;
    SSL* ssl_ = nullptr;
    std::atomic<int>* active_socket_ = nullptr;
};

std::string header_value(const std::string& headers, const char* name) {
    std::string key = name;
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    std::istringstream lines(headers);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        std::string current = line.substr(0, colon);
        std::transform(current.begin(), current.end(), current.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (current == key) {
            std::string value = line.substr(colon + 1);
            const std::size_t start = value.find_first_not_of(" \t");
            if (start == std::string::npos) {
                return std::string();
            }
            const std::size_t end = value.find_last_not_of(" \t");
            return value.substr(start, end - start + 1);
        }
    }
    return std::string();
}

int parse_status_code(const std::string& headers) {
    const std::size_t line_end = headers.find("\r\n");
    if (line_end == std::string::npos) {
        return 0;
    }
    const std::string status_line = headers.substr(0, line_end);
    const std::size_t first_space = status_line.find(' ');
    if (first_space == std::string::npos) {
        return 0;
    }
    const std::size_t second_space = status_line.find(' ', first_space + 1);
    const std::string code = status_line.substr(first_space + 1,
                                                second_space == std::string::npos
                                                    ? std::string::npos
                                                    : second_space - first_space - 1);
    try {
        return std::stoi(code);
    } catch (...) {
        return 0;
    }
}

std::string decode_icy_field(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    std::size_t i = 0;
    if (!value.empty() && value[0] == '\'') {
        ++i;
    }
    for (; i < value.size(); ++i) {
        if (value[i] == '\'') {
            break;
        }
        if (value[i] == ';') {
            break;
        }
        out.push_back(value[i]);
    }
    const std::size_t start = out.find_first_not_of(" \t");
    if (start == std::string::npos) {
        return std::string();
    }
    const std::size_t end = out.find_last_not_of(" \t");
    return out.substr(start, end - start + 1);
}

std::string parse_stream_title(const std::string& metadata) {
    std::string lower = metadata;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    const std::string key = "streamtitle=";
    const std::size_t pos = lower.find(key);
    if (pos == std::string::npos) {
        return std::string();
    }
    return decode_icy_field(metadata.substr(pos + key.size()));
}

bool stream_icy_metadata(HttpConnection& conn,
                         const std::string& headers,
                         const std::string& url,
                         IcyMetadataClient::MetadataHandler handler,
                         const std::atomic<bool>& stop_requested) {
    const std::string metaint_text = header_value(headers, "icy-metaint");
    if (metaint_text.empty()) {
        Logger::instance().debug("ICY metadata: stream has no icy-metaint: " + url);
        return false;
    }

    int metaint = 0;
    try {
        metaint = std::stoi(metaint_text);
    } catch (...) {
        return false;
    }
    if (metaint <= 0) {
        return false;
    }

    std::string last_title;
    while (!stop_requested.load(std::memory_order_relaxed)) {
        if (!conn.discard_bytes(static_cast<std::size_t>(metaint), stop_requested)) {
            break;
        }

        unsigned char meta_length = 0;
        if (!conn.read_byte(meta_length, stop_requested)) {
            break;
        }

        const std::size_t metadata_bytes = static_cast<std::size_t>(meta_length) * 16;
        if (metadata_bytes == 0) {
            continue;
        }

        std::string metadata;
        if (!conn.recv_some(metadata, metadata_bytes, stop_requested) || metadata.size() < metadata_bytes) {
            break;
        }

        const std::string title = parse_stream_title(metadata);
        if (!title.empty() && title != last_title) {
            last_title = title;
            if (handler) {
                handler(title);
            }
        }
    }
    return true;
}

} // namespace

bool IcyMetadataClient::supports_url(const std::string& url) {
    HttpEndpoint endpoint;
    return parse_http_endpoint(url, endpoint);
}

void IcyMetadataClient::stream_until_stopped(const std::string& url,
                                             MetadataHandler handler,
                                             const std::atomic<bool>& stop_requested,
                                             std::atomic<int>* active_socket) {
    std::string current_url = url;
    constexpr int kMaxRedirects = 8;

    for (int redirect = 0; redirect <= kMaxRedirects; ++redirect) {
        if (stop_requested.load(std::memory_order_relaxed)) {
            return;
        }

        HttpEndpoint endpoint;
        if (!parse_http_endpoint(current_url, endpoint)) {
            return;
        }

        HttpConnection conn;
        conn.track_active_socket(active_socket);
        if (!conn.connect(endpoint, stop_requested, active_socket)) {
            Logger::instance().debug("ICY metadata: connect failed for " + current_url);
            return;
        }

        if (!conn.send_request(endpoint)) {
            return;
        }

        std::string headers;
        if (!conn.read_headers(headers, stop_requested)) {
            return;
        }

        const int status = parse_status_code(headers);
        if (status >= 300 && status < 400) {
            const std::string location = header_value(headers, "location");
            const std::string next_url = resolve_redirect_url(current_url, location);
            if (next_url.empty() || next_url == current_url) {
                Logger::instance().debug("ICY metadata: redirect without location for " + current_url);
                return;
            }
            Logger::instance().debug("ICY metadata: redirect " + current_url + " -> " + next_url);
            current_url = next_url;
            continue;
        }

        if (status != 200 && status != 0) {
            Logger::instance().debug("ICY metadata: HTTP " + std::to_string(status) + " for " + current_url);
            return;
        }

        stream_icy_metadata(conn, headers, current_url, handler, stop_requested);
        return;
    }

    Logger::instance().debug("ICY metadata: too many redirects for " + url);
}

} // namespace pcmtp
