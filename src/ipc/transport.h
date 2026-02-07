#pragma once

#include "contracts.h"

#include <cstdint>
#include <vector>

// Abstract transport producer (used by grebe-sg).
// Sends data frames and receives control commands from the consumer.
class ITransportProducer {
public:
    virtual ~ITransportProducer() = default;

    // Send a frame (header + payload). Returns false on pipe close/error.
    virtual bool send_frame(const FrameHeaderV2& header, const void* payload) = 0;

    // Non-blocking: check for and receive a command. Returns true if a command was read.
    virtual bool receive_command(IpcCommand& cmd) = 0;
};

// Abstract transport consumer (used by grebe).
// Receives data frames and sends control commands to the producer.
class ITransportConsumer {
public:
    virtual ~ITransportConsumer() = default;

    // Blocking: read the next frame. Returns false on pipe close/error.
    virtual bool receive_frame(FrameHeaderV2& header, std::vector<int16_t>& payload) = 0;

    // Send a command to the producer. Returns false on pipe close/error.
    virtual bool send_command(const IpcCommand& cmd) = 0;
};
