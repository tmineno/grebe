#pragma once

#include "transport.h"

#include <cstdint>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#endif

/// UDP transport producer (used by grebe-sg).
/// Sends frames as single UDP datagrams to a target host:port.
/// No reverse command channel — receive_command() always returns false.
class UdpProducer : public ITransportProducer {
public:
    UdpProducer(const std::string& host, uint16_t port);
    ~UdpProducer() override;

    bool send_frame(const FrameHeaderV2& header, const void* payload) override;
    bool receive_command(IpcCommand& cmd) override;

private:
#ifdef _WIN32
    SOCKET sock_ = INVALID_SOCKET;
#else
    int sock_ = -1;
#endif
    struct sockaddr_in dest_addr_{};
    std::vector<uint8_t> send_buf_;
};

/// UDP transport consumer (used by grebe-viewer).
/// Binds to a port and receives frames as UDP datagrams.
/// No reverse command channel — send_command() always returns false.
class UdpConsumer : public ITransportConsumer {
public:
    explicit UdpConsumer(uint16_t port);
    ~UdpConsumer() override;

    bool receive_frame(FrameHeaderV2& header, std::vector<int16_t>& payload) override;
    bool send_command(const IpcCommand& cmd) override;

private:
#ifdef _WIN32
    SOCKET sock_ = INVALID_SOCKET;
#else
    int sock_ = -1;
#endif
    std::vector<uint8_t> recv_buf_;
};
