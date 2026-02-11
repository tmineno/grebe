#pragma once

#include "transport.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#include <sys/socket.h>
#endif

/// UDP transport producer (used by grebe-sg).
/// Sends frames as single UDP datagrams to a target host:port.
/// Supports scatter-gather I/O (sendmsg/WSASendTo) and optional sendmmsg batching (Linux).
/// No reverse command channel — receive_command() always returns false.
class UdpProducer : public ITransportProducer {
public:
    UdpProducer(const std::string& host, uint16_t port);
    ~UdpProducer() override;

    bool send_frame(const FrameHeaderV2& header, const void* payload) override;
    bool receive_command(IpcCommand& cmd) override;

    /// Override the maximum datagram size (default: 1400 for WSL2 safety).
    /// Set to 65000 on Windows native or real Linux for larger payloads.
    void set_max_datagram_size(size_t size) { max_datagram_size_ = size; }
    size_t max_datagram_size() const { return max_datagram_size_; }

    /// Set sendmmsg batch size (Linux only). 1 = immediate send (no batching).
    /// Must be called before first send_frame().
    void set_burst_size(size_t n);

    /// Flush any accumulated batch frames immediately.
    /// No-op if burst_size == 1 or no frames pending.
    void flush();

private:
#ifdef _WIN32
    SOCKET sock_ = INVALID_SOCKET;
#else
    int sock_ = -1;
#endif
    struct sockaddr_in dest_addr_{};
    uint64_t send_count_ = 0;
    size_t max_datagram_size_ = 1400;

    // --- sendmmsg batch state (Linux only) ---
    size_t burst_size_ = 1;
#ifndef _WIN32
    size_t batch_count_ = 0;
    std::vector<FrameHeaderV2> batch_headers_;
    std::vector<std::vector<uint8_t>> batch_payloads_;
    std::vector<struct iovec> batch_iovecs_;       // burst_size_ * 2 entries
    std::vector<struct mmsghdr> batch_mmsg_;

    bool send_scatter_gather(const FrameHeaderV2& header, const void* payload);
    void flush_internal();
#endif
};

/// UDP transport consumer (used by grebe-viewer).
/// Binds to a port and receives frames as UDP datagrams.
/// Supports recvmmsg batching (Linux) for reduced syscall overhead.
/// No reverse command channel — send_command() always returns false.
class UdpConsumer : public ITransportConsumer {
public:
    explicit UdpConsumer(uint16_t port);
    ~UdpConsumer() override;

    bool receive_frame(FrameHeaderV2& header, std::vector<int16_t>& payload) override;
    bool send_command(const IpcCommand& cmd) override;

    /// Close the socket to unblock a blocking receive_frame() call.
    void close();

    /// Set recvmmsg batch size (Linux only). 1 = single recvfrom (no batching).
    /// Must be called before first receive_frame().
    void set_burst_size(size_t n);

private:
#ifdef _WIN32
    SOCKET sock_ = INVALID_SOCKET;
#else
    int sock_ = -1;
#endif
    std::atomic<bool> closed_{false};
    uint64_t recv_count_ = 0;
    std::vector<uint8_t> recv_buf_;

    // --- recvmmsg batch state (Linux only) ---
    size_t burst_size_ = 1;
#ifndef _WIN32
    std::deque<std::pair<FrameHeaderV2, std::vector<int16_t>>> recv_queue_;
    std::vector<std::vector<uint8_t>> recv_bufs_;
    std::vector<struct iovec> recv_iovecs_;
    std::vector<struct mmsghdr> recv_mmsg_;

    bool receive_single(FrameHeaderV2& header, std::vector<int16_t>& payload);
    bool receive_batch(FrameHeaderV2& header, std::vector<int16_t>& payload);
#endif
};
