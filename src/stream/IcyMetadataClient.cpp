#include "pcmtp/stream/IcyMetadataClient.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cctype>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <algorithm>
#include <sstream>
#include <vector>

#include "pcmtp/util/Logger.hpp"

namespace pcmtp {
namespace {

struct HttpEndpoint {
    std::string host;
    std::string path;
    int port = 80;
};

bool parse_http_endpoint(const std::string& url, HttpEndpoint& out) {
    std::string normalized = url;
    if (normalized.compare(0, 6, "icy://") == 0) {
        normalized = "http://" + normalized.substr(6);
    }
    if (normalized.compare(0, 7, "http://") != 0) {
        return false;
    }

    std::size_t start = 7;
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
        out.port = 80;
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

bool send_all(int fd, const char* data, std::size_t size) {
    std::size_t sent = 0;
    while (sent < size) {
        const ssize_t chunk = ::send(fd, data + sent, size - sent, 0);
        if (chunk <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(chunk);
    }
    return true;
}

bool recv_some(int fd, std::string& buffer, std::size_t max_bytes, const std::atomic<bool>& stop_requested) {
    std::vector<char> chunk(1024);
    while (buffer.size() < max_bytes) {
        if (stop_requested.load(std::memory_order_relaxed)) {
            return false;
        }
        const ssize_t got = ::recv(fd, chunk.data(), chunk.size(), 0);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
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

bool read_headers(int fd, std::string& headers, const std::atomic<bool>& stop_requested) {
    headers.clear();
    char byte = '\0';
    while (!stop_requested.load(std::memory_order_relaxed)) {
        const ssize_t got = ::recv(fd, &byte, 1, 0);
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

std::string decode_icy_field(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '\'' && i + 1 < value.size()) {
            continue;
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
    const std::string key = "StreamTitle=";
    const std::size_t pos = metadata.find(key);
    if (pos == std::string::npos) {
        return std::string();
    }
    return decode_icy_field(metadata.substr(pos + key.size()));
}

bool discard_bytes(int fd, std::size_t bytes, const std::atomic<bool>& stop_requested) {
    std::vector<char> buffer(4096);
    std::size_t remaining = bytes;
    while (remaining > 0) {
        if (stop_requested.load(std::memory_order_relaxed)) {
            return false;
        }
        const std::size_t chunk = std::min(remaining, buffer.size());
        const ssize_t got = ::recv(fd, buffer.data(), chunk, 0);
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

int connect_endpoint(const HttpEndpoint& endpoint, const std::atomic<bool>& stop_requested) {
    if (stop_requested.load(std::memory_order_relaxed)) {
        return -1;
    }

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result = nullptr;
    const std::string port = std::to_string(endpoint.port);
    if (getaddrinfo(endpoint.host.c_str(), port.c_str(), &hints, &result) != 0 || result == nullptr) {
        return -1;
    }

    int fd = -1;
    for (addrinfo* cursor = result; cursor != nullptr; cursor = cursor->ai_next) {
        if (stop_requested.load(std::memory_order_relaxed)) {
            break;
        }
        fd = ::socket(cursor->ai_family, cursor->ai_socktype, cursor->ai_protocol);
        if (fd < 0) {
            continue;
        }

        const int flags = fcntl(fd, F_GETFL, 0);
        if (flags >= 0) {
            fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        }

        const int connect_result = ::connect(fd, cursor->ai_addr, cursor->ai_addrlen);
        if (connect_result == 0) {
            if (flags >= 0) {
                fcntl(fd, F_SETFL, flags);
            }
            break;
        }
        if (connect_result < 0 && errno != EINPROGRESS) {
            ::close(fd);
            fd = -1;
            continue;
        }

        bool connected = false;
        for (int waited_ms = 0; waited_ms < 5000 && !stop_requested.load(std::memory_order_relaxed); waited_ms += 200) {
            pollfd pfd = {fd, POLLOUT, 0};
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
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_length) != 0 || socket_error != 0) {
                break;
            }
            connected = true;
            break;
        }

        if (!connected || stop_requested.load(std::memory_order_relaxed)) {
            ::close(fd);
            fd = -1;
            continue;
        }

        if (flags >= 0) {
            fcntl(fd, F_SETFL, flags);
        }
        break;
    }
    freeaddrinfo(result);
    if (fd < 0) {
        return -1;
    }

    timeval timeout{};
    timeout.tv_sec = 2;
    timeout.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    return fd;
}

void set_active_socket(std::atomic<int>* active_socket, int fd) {
    if (active_socket != nullptr) {
        active_socket->store(fd, std::memory_order_relaxed);
    }
}

void clear_active_socket(std::atomic<int>* active_socket, int fd) {
    if (active_socket == nullptr) {
        return;
    }
    int expected = fd;
    active_socket->compare_exchange_strong(expected, -1, std::memory_order_relaxed);
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
    HttpEndpoint endpoint;
    if (!parse_http_endpoint(url, endpoint)) {
        return;
    }

    const int fd = connect_endpoint(endpoint, stop_requested);
    if (fd < 0) {
        Logger::instance().debug("ICY metadata: connect failed for " + url);
        return;
    }
    set_active_socket(active_socket, fd);

    const auto cleanup = [&]() {
        clear_active_socket(active_socket, fd);
        ::close(fd);
    };

    std::ostringstream request;
    request << "GET " << endpoint.path << " HTTP/1.0\r\n"
            << "Host: " << endpoint.host << "\r\n"
            << "User-Agent: pcm-transport/0.9\r\n"
            << "Icy-MetaData: 1\r\n"
            << "Connection: close\r\n"
            << "\r\n";
    const std::string request_text = request.str();
    if (!send_all(fd, request_text.c_str(), request_text.size())) {
        cleanup();
        return;
    }

    std::string headers;
    if (!read_headers(fd, headers, stop_requested)) {
        cleanup();
        return;
    }

    const std::string metaint_text = header_value(headers, "icy-metaint");
    if (metaint_text.empty()) {
        Logger::instance().debug("ICY metadata: stream has no icy-metaint: " + url);
        cleanup();
        return;
    }

    int metaint = 0;
    try {
        metaint = std::stoi(metaint_text);
    } catch (...) {
        cleanup();
        return;
    }
    if (metaint <= 0) {
        cleanup();
        return;
    }

    std::string last_title;
  while (!stop_requested.load(std::memory_order_relaxed)) {
        if (!discard_bytes(fd, static_cast<std::size_t>(metaint), stop_requested)) {
            break;
        }

        unsigned char meta_length = 0;
        const ssize_t length_read = ::recv(fd, &meta_length, 1, 0);
        if (length_read <= 0) {
            break;
        }

        const std::size_t metadata_bytes = static_cast<std::size_t>(meta_length) * 16;
        if (metadata_bytes == 0) {
            continue;
        }

        std::string metadata;
        if (!recv_some(fd, metadata, metadata_bytes, stop_requested) || metadata.size() < metadata_bytes) {
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

    cleanup();
}

} // namespace pcmtp
