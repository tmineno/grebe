#include "pipe_transport.h"

#include <spdlog/spdlog.h>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <poll.h>
#include <sys/uio.h>
#include <unistd.h>
#endif

// =========================================================================
// Helpers: read/write all bytes (handles partial read/write)
// =========================================================================

namespace {

bool write_all(int fd, const void* buf, size_t len) {
    const char* p = static_cast<const char*>(buf);
    while (len > 0) {
#ifdef _WIN32
        int n = _write(fd, p, static_cast<unsigned int>(len));
#else
        ssize_t n = ::write(fd, p, len);
#endif
        if (n <= 0) return false;
        p += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

bool read_all(int fd, void* buf, size_t len) {
    char* p = static_cast<char*>(buf);
    while (len > 0) {
#ifdef _WIN32
        int n = _read(fd, p, static_cast<unsigned int>(len));
#else
        ssize_t n = ::read(fd, p, len);
#endif
        if (n <= 0) return false;
        p += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

} // namespace

// =========================================================================
// PipeProducer (grebe-sg side)
// =========================================================================

PipeProducer::PipeProducer()
#ifdef _WIN32
    : write_fd_(_fileno(stdout))
    , read_fd_(_fileno(stdin))
{
    _setmode(write_fd_, _O_BINARY);
    _setmode(read_fd_, _O_BINARY);
}
#else
    : write_fd_(STDOUT_FILENO)
    , read_fd_(STDIN_FILENO)
{
}
#endif

bool PipeProducer::send_frame(const FrameHeaderV2& header, const void* payload) {
#ifndef _WIN32
    // Use writev to send header+payload in a single syscall
    struct iovec iov[2];
    iov[0].iov_base = const_cast<void*>(static_cast<const void*>(&header));
    iov[0].iov_len = sizeof(header);
    int iovcnt = 1;
    size_t total = sizeof(header);
    if (header.payload_bytes > 0 && payload) {
        iov[1].iov_base = const_cast<void*>(payload);
        iov[1].iov_len = header.payload_bytes;
        iovcnt = 2;
        total += header.payload_bytes;
    }
    size_t written = 0;
    while (written < total) {
        ssize_t n = ::writev(write_fd_, iov, iovcnt);
        if (n <= 0) return false;
        written += static_cast<size_t>(n);
        // Advance iovecs past completed segments
        size_t adv = static_cast<size_t>(n);
        while (iovcnt > 0 && adv >= iov[0].iov_len) {
            adv -= iov[0].iov_len;
            iov[0] = iov[1];
            iovcnt--;
        }
        if (iovcnt > 0 && adv > 0) {
            iov[0].iov_base = static_cast<char*>(iov[0].iov_base) + adv;
            iov[0].iov_len -= adv;
        }
    }
    return true;
#else
    if (!write_all(write_fd_, &header, sizeof(header))) return false;
    if (header.payload_bytes > 0 && payload) {
        if (!write_all(write_fd_, payload, header.payload_bytes)) return false;
    }
    return true;
#endif
}

bool PipeProducer::receive_command(IpcCommand& cmd) {
#ifdef _WIN32
    HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(read_fd_));
    if (h == INVALID_HANDLE_VALUE) return false;

    DWORD avail = 0;
    if (!PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr)) return false;
    if (avail < sizeof(IpcCommand)) return false;

    if (!read_all(read_fd_, &cmd, sizeof(cmd))) return false;
    if (cmd.magic != IPC_COMMAND_MAGIC) {
        spdlog::warn("PipeProducer: invalid command magic 0x{:08x}", cmd.magic);
        return false;
    }
    return true;
#else
    struct pollfd pfd{};
    pfd.fd = read_fd_;
    pfd.events = POLLIN;

    int ret = poll(&pfd, 1, 0);  // non-blocking
    if (ret <= 0) return false;

    if (!read_all(read_fd_, &cmd, sizeof(cmd))) return false;
    if (cmd.magic != IPC_COMMAND_MAGIC) {
        spdlog::warn("PipeProducer: invalid command magic 0x{:08x}", cmd.magic);
        return false;
    }
    return true;
#endif
}

// =========================================================================
// PipeConsumer (grebe side)
// =========================================================================

PipeConsumer::PipeConsumer(int read_fd, int write_fd)
    : read_fd_(read_fd)
    , write_fd_(write_fd)
{
#ifdef _WIN32
    _setmode(read_fd_, _O_BINARY);
    _setmode(write_fd_, _O_BINARY);
#endif
}

PipeConsumer::~PipeConsumer() {
#ifdef _WIN32
    if (read_fd_ >= 0) _close(read_fd_);
    if (write_fd_ >= 0) _close(write_fd_);
#else
    if (read_fd_ >= 0) close(read_fd_);
    if (write_fd_ >= 0) close(write_fd_);
#endif
    read_fd_ = -1;
    write_fd_ = -1;
}

bool PipeConsumer::receive_frame(FrameHeaderV2& header, std::vector<int16_t>& payload) {
    if (!read_all(read_fd_, &header, sizeof(header))) return false;

    if (header.magic != FRAME_HEADER_MAGIC) {
        spdlog::warn("PipeConsumer: invalid frame magic 0x{:08x}", header.magic);
        return false;
    }

    if (header.payload_bytes > 0) {
        payload.resize(header.payload_bytes / sizeof(int16_t));
        if (!read_all(read_fd_, payload.data(), header.payload_bytes)) return false;
    } else {
        payload.clear();
    }
    return true;
}

bool PipeConsumer::send_command(const IpcCommand& cmd) {
    return write_all(write_fd_, &cmd, sizeof(cmd));
}
