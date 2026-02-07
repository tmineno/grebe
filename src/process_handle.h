#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Cross-platform child process handle.
// Spawns, monitors, and terminates a child process.
class ProcessHandle {
public:
    ProcessHandle() = default;
    ~ProcessHandle();

    ProcessHandle(const ProcessHandle&) = delete;
    ProcessHandle& operator=(const ProcessHandle&) = delete;
    ProcessHandle(ProcessHandle&& other) noexcept;
    ProcessHandle& operator=(ProcessHandle&& other) noexcept;

    // Spawn a child process. Returns true on success.
    // exe: path to executable
    // args: command-line arguments (excluding argv[0])
    bool spawn(const std::string& exe, const std::vector<std::string>& args = {});

    // Check if the child is still running.
    bool is_alive() const;

    // Block until the child exits. Returns exit code.
    int wait();

    // Non-blocking wait. Returns exit code if exited, -1 if still running.
    int try_wait();

    // Send termination signal (SIGTERM on POSIX, TerminateProcess on Windows).
    void terminate();

    // Get the child PID (0 if not spawned).
    uint64_t pid() const;

private:
    void reset();

#ifdef _WIN32
    void* process_handle_ = nullptr;  // HANDLE
#else
    int pid_ = 0;
#endif
    bool exited_ = false;
    int exit_code_ = 0;
};
