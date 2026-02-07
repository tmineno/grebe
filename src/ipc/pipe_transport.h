#pragma once

#include "transport.h"

// Pipe-based transport producer (used by grebe-sg).
// Writes frames to stdout (fd 1), reads commands from stdin (fd 0).
class PipeProducer : public ITransportProducer {
public:
    PipeProducer();
    ~PipeProducer() override = default;

    bool send_frame(const FrameHeaderV2& header, const void* payload) override;
    bool receive_command(IpcCommand& cmd) override;

private:
    int write_fd_;  // stdout
    int read_fd_;   // stdin
};

// Pipe-based transport consumer (used by grebe).
// Reads frames from a pipe fd, writes commands to another pipe fd.
class PipeConsumer : public ITransportConsumer {
public:
    // read_fd: connected to child's stdout; write_fd: connected to child's stdin.
    PipeConsumer(int read_fd, int write_fd);
    ~PipeConsumer() override;

    bool receive_frame(FrameHeaderV2& header, std::vector<int16_t>& payload) override;
    bool send_command(const IpcCommand& cmd) override;

private:
    int read_fd_;
    int write_fd_;
};
