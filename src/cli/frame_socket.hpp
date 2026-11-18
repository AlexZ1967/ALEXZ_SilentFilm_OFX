#pragma once

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <unistd.h>
#endif
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>

namespace silentfilm::offline {

/** Own a nonblocking local connection with bounded waits and cooperative cancellation. */
class FrameSocket {
#ifdef _WIN32
    using Socket = SOCKET;
    static constexpr Socket invalid = INVALID_SOCKET;
    bool initialized_ = false;
#else
    using Socket = int;
    static constexpr Socket invalid = -1;
#endif
    Socket socket_ = invalid;
    static int errorCode() {
#ifdef _WIN32
        return WSAGetLastError();
#else
        return errno;
#endif
    }
    static bool retry(int code) {
#ifdef _WIN32
        return code == WSAEINTR || code == WSAEWOULDBLOCK || code == WSAEINPROGRESS;
#else
        return code == EINTR || code == EAGAIN || code == EWOULDBLOCK || code == EINPROGRESS;
#endif
    }
    void create(int family) {
#ifdef _WIN32
        WSADATA data{};
        const int status = WSAStartup(MAKEWORD(2, 2), &data);
        if (status) throw std::runtime_error("Cannot initialize Winsock: " + std::to_string(status));
        initialized_ = true;
#endif
        socket_ = ::socket(family, SOCK_STREAM, 0);
        if (socket_ == invalid) throw std::runtime_error("Cannot create Resolve frame socket");
#ifdef _WIN32
        u_long nonblocking = 1;
        if (ioctlsocket(socket_, FIONBIO, &nonblocking)) throw std::runtime_error("Cannot configure frame socket");
#else
        const int flags = fcntl(socket_, F_GETFL, 0);
        if (flags < 0 || fcntl(socket_, F_SETFL, flags | O_NONBLOCK) < 0)
            throw std::runtime_error("Cannot configure frame socket");
#endif
    }
    bool wait(bool sending) {
#ifdef _WIN32
        fd_set ready, failed;
        FD_ZERO(&ready); FD_ZERO(&failed);
        FD_SET(socket_, &ready); FD_SET(socket_, &failed);
        timeval timeout{0, 200000};
        const int result = ::select(0, sending ? nullptr : &ready, sending ? &ready : nullptr, &failed, &timeout);
#else
        pollfd descriptor{socket_, static_cast<short>(sending ? POLLOUT : POLLIN), 0};
        const int result = ::poll(&descriptor, 1, 200);
#endif
        if (result < 0 && retry(errorCode())) return false;
        if (result < 0) throw std::runtime_error("Resolve frame socket wait failed");
        return result > 0;
    }
    void connect(const sockaddr* address, int size, const std::function<bool()>& cancelled) {
        if (::connect(socket_, address, size) == 0) return;
        const int connectionError = errorCode();
        if (!retry(connectionError)) throw std::runtime_error("Resolve frame provider is unavailable: " + std::to_string(connectionError));
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!wait(true)) {
            if (cancelled()) throw std::runtime_error("Cancelled");
            if (std::chrono::steady_clock::now() > deadline) throw std::runtime_error("Resolve frame connect timed out");
        }
        int status = 0;
#ifdef _WIN32
        int length = sizeof(status);
        const int result = getsockopt(socket_, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&status), &length);
#else
        socklen_t length = sizeof(status);
        const int result = getsockopt(socket_, SOL_SOCKET, SO_ERROR, &status, &length);
#endif
        if (result || status) throw std::runtime_error("Resolve frame provider connection failed: " + std::to_string(status));
    }
public:
    FrameSocket() = default;
    FrameSocket(const FrameSocket&) = delete;
    FrameSocket& operator=(const FrameSocket&) = delete;
    ~FrameSocket() {
#ifdef _WIN32
        if (socket_ != invalid) closesocket(socket_);
        if (initialized_) WSACleanup();
#else
        if (socket_ != invalid) ::close(socket_);
#endif
    }
    void loopback(int port, const std::function<bool()>& cancelled) {
        if (port < 1 || port > 65535) throw std::runtime_error("Invalid frame provider TCP port");
        create(AF_INET);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(static_cast<unsigned short>(port));
        connect(reinterpret_cast<const sockaddr*>(&address), sizeof(address), cancelled);
    }
    void abstract(const std::string& name, const std::function<bool()>& cancelled) {
#if defined(__linux__)
        sockaddr_un address{};
        if (name.empty() || name.size() + 1 > sizeof(address.sun_path)) throw std::runtime_error("Invalid frame socket name");
        create(AF_UNIX);
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path + 1, name.data(), name.size());
        connect(reinterpret_cast<const sockaddr*>(&address), static_cast<int>(offsetof(sockaddr_un, sun_path) + 1 + name.size()), cancelled);
#else
        (void)name; (void)cancelled;
        throw std::runtime_error("Linux frame source cannot be opened on this platform; start a new job");
#endif
    }
    void transfer(void* data, std::size_t bytes, bool sending, const std::function<bool()>& cancelled) {
        auto* position = static_cast<char*>(data);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(5);
        while (bytes) {
            if (cancelled()) throw std::runtime_error("Cancelled");
            if (std::chrono::steady_clock::now() > deadline) throw std::runtime_error("Resolve frame request timed out");
            if (!wait(sending)) continue;
            const auto count = static_cast<int>(std::min<std::size_t>(bytes, INT_MAX));
#ifdef _WIN32
            const auto amount = sending ? ::send(socket_, position, count, 0) : ::recv(socket_, position, count, 0);
#else
            const auto amount = sending ? ::send(socket_, position, count, MSG_NOSIGNAL) : ::recv(socket_, position, count, 0);
#endif
            if (amount < 0 && retry(errorCode())) continue;
            if (amount <= 0) throw std::runtime_error("Resolve frame provider disconnected; see resolve_frames.log");
            position += amount; bytes -= static_cast<std::size_t>(amount);
        }
    }
};
} // namespace silentfilm::offline
