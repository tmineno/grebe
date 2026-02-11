#include "udp_transport.h"

#include <spdlog/spdlog.h>

#include <cstring>

#ifdef _WIN32
#pragma comment(lib, "ws2_32.lib")
#include <winsock2.h>
#include <ws2tcpip.h>

namespace {
struct WinsockInit {
    WinsockInit() {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
    }
    ~WinsockInit() { WSACleanup(); }
};
static WinsockInit g_winsock_init;
} // namespace

static void close_socket(SOCKET s) {
    if (s != INVALID_SOCKET) closesocket(s);
}
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

static void close_socket(int s) {
    if (s >= 0) ::close(s);
}
#endif

// =========================================================================
// UdpProducer (grebe-sg side)
// =========================================================================

UdpProducer::UdpProducer(const std::string& host, uint16_t port) {
    sock_ = ::socket(AF_INET, SOCK_DGRAM, 0);
#ifdef _WIN32
    if (sock_ == INVALID_SOCKET) {
        spdlog::error("UdpProducer: socket() failed: {}", WSAGetLastError());
        return;
    }
#else
    if (sock_ < 0) {
        spdlog::error("UdpProducer: socket() failed: {}", std::strerror(errno));
        return;
    }
#endif

    // Set send buffer to 1 MB
    int sndbuf = 1 * 1024 * 1024;
    setsockopt(sock_, SOL_SOCKET, SO_SNDBUF,
               reinterpret_cast<const char*>(&sndbuf), sizeof(sndbuf));

    std::memset(&dest_addr_, 0, sizeof(dest_addr_));
    dest_addr_.sin_family = AF_INET;
    dest_addr_.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &dest_addr_.sin_addr) != 1) {
        spdlog::error("UdpProducer: invalid address '{}'", host);
        close_socket(sock_);
#ifdef _WIN32
        sock_ = INVALID_SOCKET;
#else
        sock_ = -1;
#endif
        return;
    }

    spdlog::info("UdpProducer: target {}:{}", host, port);
}

UdpProducer::~UdpProducer() {
    close_socket(sock_);
}

bool UdpProducer::send_frame(const FrameHeaderV2& header, const void* payload) {
#ifdef _WIN32
    if (sock_ == INVALID_SOCKET) return false;
#else
    if (sock_ < 0) return false;
#endif

    size_t total = sizeof(header) + header.payload_bytes;
    if (total > max_datagram_size_) {
        spdlog::warn("UdpProducer: datagram too large ({} bytes, max {}), dropping",
                     total, max_datagram_size_);
        return false;
    }

    // Build single send buffer: [header][payload]
    send_buf_.resize(total);
    std::memcpy(send_buf_.data(), &header, sizeof(header));
    if (header.payload_bytes > 0 && payload) {
        std::memcpy(send_buf_.data() + sizeof(header), payload, header.payload_bytes);
    }

#ifdef _WIN32
    int sent = sendto(sock_,
                      reinterpret_cast<const char*>(send_buf_.data()),
                      static_cast<int>(total), 0,
                      reinterpret_cast<const struct sockaddr*>(&dest_addr_),
                      sizeof(dest_addr_));
    if (sent == SOCKET_ERROR) {
        spdlog::warn("UdpProducer: sendto failed: {}", WSAGetLastError());
        return false;
    }
#else
    ssize_t sent = sendto(sock_, send_buf_.data(), total, 0,
                          reinterpret_cast<const struct sockaddr*>(&dest_addr_),
                          sizeof(dest_addr_));
    if (sent < 0) {
        spdlog::warn("UdpProducer: sendto failed: {}", std::strerror(errno));
        return false;
    }
#endif

    ++send_count_;
    if (send_count_ == 1) {
        spdlog::info("UdpProducer: first frame sent (seq={}, {} bytes)",
                     header.sequence, total);
    }

    return true;
}

bool UdpProducer::receive_command(IpcCommand& /*cmd*/) {
    // No reverse command channel for UDP transport
    return false;
}

// =========================================================================
// UdpConsumer (grebe-viewer side)
// =========================================================================

UdpConsumer::UdpConsumer(uint16_t port) {
    sock_ = ::socket(AF_INET, SOCK_DGRAM, 0);
#ifdef _WIN32
    if (sock_ == INVALID_SOCKET) {
        spdlog::error("UdpConsumer: socket() failed: {}", WSAGetLastError());
        return;
    }
#else
    if (sock_ < 0) {
        spdlog::error("UdpConsumer: socket() failed: {}", std::strerror(errno));
        return;
    }
#endif

    // Allow quick rebind after restart
    int reuse = 1;
    setsockopt(sock_, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    // Set receive buffer to 4 MB
    int rcvbuf = 4 * 1024 * 1024;
    setsockopt(sock_, SOL_SOCKET, SO_RCVBUF,
               reinterpret_cast<const char*>(&rcvbuf), sizeof(rcvbuf));

    // Set receive timeout (500ms) so ingestion thread can check stop_requested
#ifdef _WIN32
    DWORD timeout_ms = 500;
    setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
#else
    struct timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = 500'000;  // 500ms
    setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&tv), sizeof(tv));
#endif

    struct sockaddr_in bind_addr{};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = INADDR_ANY;
    bind_addr.sin_port = htons(port);

    if (::bind(sock_, reinterpret_cast<const struct sockaddr*>(&bind_addr),
               sizeof(bind_addr)) < 0) {
#ifdef _WIN32
        spdlog::error("UdpConsumer: bind failed on port {}: {}", port, WSAGetLastError());
        closesocket(sock_);
        sock_ = INVALID_SOCKET;
#else
        spdlog::error("UdpConsumer: bind failed on port {}: {}", port, std::strerror(errno));
        ::close(sock_);
        sock_ = -1;
#endif
        return;
    }

    // Pre-allocate receive buffer (max UDP datagram)
    recv_buf_.resize(65536);

    spdlog::info("UdpConsumer: listening on port {}", port);
}

UdpConsumer::~UdpConsumer() {
    close();
}

void UdpConsumer::close() {
    closed_.store(true, std::memory_order_release);
#ifdef _WIN32
    if (sock_ != INVALID_SOCKET) {
        closesocket(sock_);
        sock_ = INVALID_SOCKET;
    }
#else
    if (sock_ >= 0) {
        ::close(sock_);
        sock_ = -1;
    }
#endif
}

bool UdpConsumer::receive_frame(FrameHeaderV2& header, std::vector<int16_t>& payload) {
    while (!closed_.load(std::memory_order_acquire)) {
#ifdef _WIN32
        if (sock_ == INVALID_SOCKET) return false;

        int received = recvfrom(sock_,
                                reinterpret_cast<char*>(recv_buf_.data()),
                                static_cast<int>(recv_buf_.size()), 0,
                                nullptr, nullptr);
        if (received == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err == WSAETIMEDOUT) continue;  // timeout, retry
            if (closed_.load(std::memory_order_relaxed)) return false;
            spdlog::warn("UdpConsumer: recvfrom error: {}", err);
            return false;
        }
        if (received < static_cast<int>(sizeof(FrameHeaderV2))) {
            continue;  // too small, skip
        }
        size_t nbytes = static_cast<size_t>(received);
#else
        if (sock_ < 0) return false;

        ssize_t received = recvfrom(sock_, recv_buf_.data(), recv_buf_.size(), 0,
                                    nullptr, nullptr);
        if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;  // timeout, retry
            if (closed_.load(std::memory_order_relaxed)) return false;
            spdlog::warn("UdpConsumer: recvfrom error: {}", std::strerror(errno));
            return false;
        }
        if (received < static_cast<ssize_t>(sizeof(FrameHeaderV2))) {
            continue;  // too small, skip
        }
        size_t nbytes = static_cast<size_t>(received);
#endif

        // Extract header
        std::memcpy(&header, recv_buf_.data(), sizeof(header));

        if (header.magic != FRAME_HEADER_MAGIC) {
            spdlog::warn("UdpConsumer: invalid frame magic 0x{:08x}", header.magic);
            continue;  // bad frame, skip
        }

        // Extract payload
        size_t payload_offset = sizeof(FrameHeaderV2);
        size_t payload_available = nbytes - payload_offset;

        if (header.payload_bytes > 0 && payload_available >= header.payload_bytes) {
            size_t num_samples = header.payload_bytes / sizeof(int16_t);
            payload.resize(num_samples);
            std::memcpy(payload.data(), recv_buf_.data() + payload_offset,
                        header.payload_bytes);
        } else {
            payload.clear();
        }

        ++recv_count_;
        if (recv_count_ == 1) {
            spdlog::info("UdpConsumer: first frame received (seq={}, {}ch, {} samples/ch, {} bytes)",
                         header.sequence, header.channel_count,
                         header.block_length_samples, nbytes);
        }

        return true;
    }
    return false;  // closed
}

bool UdpConsumer::send_command(const IpcCommand& /*cmd*/) {
    // No reverse command channel for UDP transport
    return false;
}
