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
#ifndef _WIN32
    flush();
#endif
    close_socket(sock_);
}

void UdpProducer::set_burst_size(size_t n) {
    if (n < 1) n = 1;
    burst_size_ = n;
#ifndef _WIN32
    if (burst_size_ > 1) {
        batch_headers_.resize(burst_size_);
        batch_payloads_.resize(burst_size_);
        batch_iovecs_.resize(burst_size_ * 2);
        batch_mmsg_.resize(burst_size_);
        batch_count_ = 0;
    }
#endif
}

void UdpProducer::flush() {
#ifndef _WIN32
    flush_internal();
#endif
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

#ifdef _WIN32
    // Windows: scatter-gather via WSASendTo (no intermediate buffer)
    WSABUF bufs[2];
    bufs[0].buf = reinterpret_cast<char*>(const_cast<FrameHeaderV2*>(&header));
    bufs[0].len = static_cast<ULONG>(sizeof(header));
    DWORD buf_count = 1;
    if (header.payload_bytes > 0 && payload) {
        bufs[1].buf = reinterpret_cast<char*>(const_cast<void*>(payload));
        bufs[1].len = static_cast<ULONG>(header.payload_bytes);
        buf_count = 2;
    }
    DWORD sent_bytes = 0;
    int result = WSASendTo(sock_, bufs, buf_count, &sent_bytes, 0,
                           reinterpret_cast<const struct sockaddr*>(&dest_addr_),
                           sizeof(dest_addr_), nullptr, nullptr);
    if (result == SOCKET_ERROR) {
        spdlog::warn("UdpProducer: WSASendTo failed: {}", WSAGetLastError());
        return false;
    }
#else
    // Linux: scatter-gather or sendmmsg batch
    if (burst_size_ > 1) {
        // Batch mode: accumulate frame, flush when full
        batch_headers_[batch_count_] = header;
        batch_payloads_[batch_count_].resize(header.payload_bytes);
        if (header.payload_bytes > 0 && payload) {
            std::memcpy(batch_payloads_[batch_count_].data(), payload, header.payload_bytes);
        }

        // Set up iovec pair for this frame
        size_t iov_idx = batch_count_ * 2;
        batch_iovecs_[iov_idx].iov_base = &batch_headers_[batch_count_];
        batch_iovecs_[iov_idx].iov_len = sizeof(FrameHeaderV2);
        batch_iovecs_[iov_idx + 1].iov_base = batch_payloads_[batch_count_].data();
        batch_iovecs_[iov_idx + 1].iov_len = header.payload_bytes;

        // Set up mmsghdr for this frame
        std::memset(&batch_mmsg_[batch_count_], 0, sizeof(struct mmsghdr));
        batch_mmsg_[batch_count_].msg_hdr.msg_name = &dest_addr_;
        batch_mmsg_[batch_count_].msg_hdr.msg_namelen = sizeof(dest_addr_);
        batch_mmsg_[batch_count_].msg_hdr.msg_iov = &batch_iovecs_[iov_idx];
        batch_mmsg_[batch_count_].msg_hdr.msg_iovlen =
            (header.payload_bytes > 0) ? 2 : 1;

        ++batch_count_;
        ++send_count_;

        if (batch_count_ >= burst_size_) {
            flush_internal();
        }

        if (send_count_ == 1) {
            spdlog::info("UdpProducer: first frame queued (seq={}, {} bytes, burst={})",
                         header.sequence, total, burst_size_);
        }
        return true;
    }

    // Non-batch mode: scatter-gather sendmsg
    return send_scatter_gather(header, payload);
#endif

#ifdef _WIN32
    ++send_count_;
    if (send_count_ == 1) {
        spdlog::info("UdpProducer: first frame sent (seq={}, {} bytes)",
                     header.sequence, total);
    }
    return true;
#endif
}

#ifndef _WIN32
bool UdpProducer::send_scatter_gather(const FrameHeaderV2& header, const void* payload) {
    struct iovec iov[2];
    iov[0].iov_base = const_cast<void*>(static_cast<const void*>(&header));
    iov[0].iov_len = sizeof(header);

    struct msghdr msg{};
    msg.msg_name = &dest_addr_;
    msg.msg_namelen = sizeof(dest_addr_);
    msg.msg_iov = iov;

    if (header.payload_bytes > 0 && payload) {
        iov[1].iov_base = const_cast<void*>(payload);
        iov[1].iov_len = header.payload_bytes;
        msg.msg_iovlen = 2;
    } else {
        msg.msg_iovlen = 1;
    }

    ssize_t sent = sendmsg(sock_, &msg, 0);
    if (sent < 0) {
        spdlog::warn("UdpProducer: sendmsg failed: {}", std::strerror(errno));
        return false;
    }

    ++send_count_;
    if (send_count_ == 1) {
        spdlog::info("UdpProducer: first frame sent (seq={}, {} bytes)",
                     header.sequence, sizeof(header) + header.payload_bytes);
    }
    return true;
}

void UdpProducer::flush_internal() {
    if (batch_count_ == 0) return;

    size_t remaining = batch_count_;
    size_t offset = 0;

    while (remaining > 0) {
        int sent = sendmmsg(sock_, &batch_mmsg_[offset],
                            static_cast<unsigned int>(remaining), 0);
        if (sent < 0) {
            spdlog::warn("UdpProducer: sendmmsg failed: {}", std::strerror(errno));
            break;
        }
        offset += static_cast<size_t>(sent);
        remaining -= static_cast<size_t>(sent);
    }

    batch_count_ = 0;
}
#endif

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

void UdpConsumer::set_burst_size(size_t n) {
    if (n < 1) n = 1;
    burst_size_ = n;
#ifndef _WIN32
    if (burst_size_ > 1) {
        recv_bufs_.resize(burst_size_);
        recv_iovecs_.resize(burst_size_);
        recv_mmsg_.resize(burst_size_);
        for (size_t i = 0; i < burst_size_; ++i) {
            recv_bufs_[i].resize(65536);
            recv_iovecs_[i].iov_base = recv_bufs_[i].data();
            recv_iovecs_[i].iov_len = recv_bufs_[i].size();
            std::memset(&recv_mmsg_[i], 0, sizeof(struct mmsghdr));
            recv_mmsg_[i].msg_hdr.msg_iov = &recv_iovecs_[i];
            recv_mmsg_[i].msg_hdr.msg_iovlen = 1;
        }
    }
#endif
}

bool UdpConsumer::receive_frame(FrameHeaderV2& header, std::vector<int16_t>& payload) {
#ifndef _WIN32
    if (burst_size_ > 1) {
        return receive_batch(header, payload);
    }
    return receive_single(header, payload);
#else
    // Windows: single recvfrom (same as original)
    while (!closed_.load(std::memory_order_acquire)) {
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
#endif
}

#ifndef _WIN32
bool UdpConsumer::receive_single(FrameHeaderV2& header, std::vector<int16_t>& payload) {
    while (!closed_.load(std::memory_order_acquire)) {
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

bool UdpConsumer::receive_batch(FrameHeaderV2& header, std::vector<int16_t>& payload) {
    while (!closed_.load(std::memory_order_acquire)) {
        // Return from internal queue first
        if (!recv_queue_.empty()) {
            auto& front = recv_queue_.front();
            header = front.first;
            payload = std::move(front.second);
            recv_queue_.pop_front();
            return true;
        }

        if (sock_ < 0) return false;

        // Bulk receive with recvmmsg (MSG_WAITFORONE: block for first, non-blocking for rest)
        int n = recvmmsg(sock_, recv_mmsg_.data(),
                         static_cast<unsigned int>(burst_size_),
                         MSG_WAITFORONE, nullptr);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;  // timeout, retry
            if (closed_.load(std::memory_order_relaxed)) return false;
            spdlog::warn("UdpConsumer: recvmmsg error: {}", std::strerror(errno));
            return false;
        }

        // Parse received datagrams into queue
        for (int i = 0; i < n; ++i) {
            size_t nbytes = recv_mmsg_[i].msg_len;
            if (nbytes < sizeof(FrameHeaderV2)) continue;

            FrameHeaderV2 hdr;
            std::memcpy(&hdr, recv_bufs_[i].data(), sizeof(hdr));

            if (hdr.magic != FRAME_HEADER_MAGIC) continue;

            std::vector<int16_t> pl;
            size_t payload_offset = sizeof(FrameHeaderV2);
            size_t payload_available = nbytes - payload_offset;

            if (hdr.payload_bytes > 0 && payload_available >= hdr.payload_bytes) {
                size_t num_samples = hdr.payload_bytes / sizeof(int16_t);
                pl.resize(num_samples);
                std::memcpy(pl.data(), recv_bufs_[i].data() + payload_offset,
                            hdr.payload_bytes);
            }

            ++recv_count_;
            if (recv_count_ == 1) {
                spdlog::info("UdpConsumer: first frame received (seq={}, {}ch, {} samples/ch, {} bytes, batch={})",
                             hdr.sequence, hdr.channel_count,
                             hdr.block_length_samples, nbytes, burst_size_);
            }

            recv_queue_.emplace_back(hdr, std::move(pl));
        }

        // Re-initialize mmsghdr for next batch (msg_len is written by kernel)
        for (size_t i = 0; i < burst_size_; ++i) {
            recv_mmsg_[i].msg_len = 0;
        }
    }
    return false;  // closed
}
#endif

bool UdpConsumer::send_command(const IpcCommand& /*cmd*/) {
    // No reverse command channel for UDP transport
    return false;
}
