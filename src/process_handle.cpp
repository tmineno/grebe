#include "process_handle.h"

#include <spdlog/spdlog.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <cstring>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

ProcessHandle::~ProcessHandle() {
    // If child is still running, terminate it
    if (is_alive()) {
        terminate();
        wait();
    }
    reset();
}

ProcessHandle::ProcessHandle(ProcessHandle&& other) noexcept
#ifdef _WIN32
    : process_handle_(other.process_handle_)
#else
    : pid_(other.pid_)
#endif
    , exited_(other.exited_)
    , exit_code_(other.exit_code_)
{
#ifdef _WIN32
    other.process_handle_ = nullptr;
#else
    other.pid_ = 0;
#endif
    other.exited_ = false;
    other.exit_code_ = 0;
}

ProcessHandle& ProcessHandle::operator=(ProcessHandle&& other) noexcept {
    if (this != &other) {
        if (is_alive()) {
            terminate();
            wait();
        }
        reset();

#ifdef _WIN32
        process_handle_ = other.process_handle_;
        other.process_handle_ = nullptr;
#else
        pid_ = other.pid_;
        other.pid_ = 0;
#endif
        exited_ = other.exited_;
        exit_code_ = other.exit_code_;
        other.exited_ = false;
        other.exit_code_ = 0;
    }
    return *this;
}

void ProcessHandle::reset() {
#ifdef _WIN32
    if (process_handle_) {
        CloseHandle(process_handle_);
        process_handle_ = nullptr;
    }
#else
    pid_ = 0;
#endif
    exited_ = false;
    exit_code_ = 0;
}

#ifdef _WIN32

bool ProcessHandle::spawn(const std::string& exe, const std::vector<std::string>& args) {
    reset();

    // Build command line: "exe" arg1 arg2 ...
    std::string cmd_line = "\"" + exe + "\"";
    for (const auto& arg : args) {
        cmd_line += " " + arg;
    }

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    BOOL ok = CreateProcessA(
        nullptr,
        cmd_line.data(),
        nullptr, nullptr,
        FALSE,
        0,
        nullptr, nullptr,
        &si, &pi
    );

    if (!ok) {
        spdlog::error("ProcessHandle::spawn failed: CreateProcess error {}", GetLastError());
        return false;
    }

    CloseHandle(pi.hThread);
    process_handle_ = pi.hProcess;
    spdlog::info("ProcessHandle: spawned PID {} ({})", pi.dwProcessId, exe);
    return true;
}

bool ProcessHandle::is_alive() const {
    if (!process_handle_ || exited_) return false;
    DWORD result = WaitForSingleObject(process_handle_, 0);
    return result == WAIT_TIMEOUT;
}

int ProcessHandle::wait() {
    if (!process_handle_) return exit_code_;
    if (exited_) return exit_code_;

    WaitForSingleObject(process_handle_, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(process_handle_, &code);
    exit_code_ = static_cast<int>(code);
    exited_ = true;
    return exit_code_;
}

int ProcessHandle::try_wait() {
    if (!process_handle_) return exit_code_;
    if (exited_) return exit_code_;

    DWORD result = WaitForSingleObject(process_handle_, 0);
    if (result == WAIT_OBJECT_0) {
        DWORD code = 0;
        GetExitCodeProcess(process_handle_, &code);
        exit_code_ = static_cast<int>(code);
        exited_ = true;
        return exit_code_;
    }
    return -1;
}

void ProcessHandle::terminate() {
    if (process_handle_ && !exited_) {
        TerminateProcess(process_handle_, 1);
    }
}

bool ProcessHandle::spawn_with_pipes(const std::string& exe, const std::vector<std::string>& args,
                                     int& child_stdin_write_fd, int& child_stdout_read_fd) {
    reset();
    child_stdin_write_fd = -1;
    child_stdout_read_fd = -1;

    // Create pipes for stdin and stdout
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE stdin_read = nullptr, stdin_write = nullptr;
    HANDLE stdout_read = nullptr, stdout_write = nullptr;

    if (!CreatePipe(&stdin_read, &stdin_write, &sa, 0)) {
        spdlog::error("ProcessHandle::spawn_with_pipes: CreatePipe(stdin) failed: {}", GetLastError());
        return false;
    }
    // Parent's write end should not be inherited
    SetHandleInformation(stdin_write, HANDLE_FLAG_INHERIT, 0);

    if (!CreatePipe(&stdout_read, &stdout_write, &sa, 0)) {
        spdlog::error("ProcessHandle::spawn_with_pipes: CreatePipe(stdout) failed: {}", GetLastError());
        CloseHandle(stdin_read);
        CloseHandle(stdin_write);
        return false;
    }
    // Parent's read end should not be inherited
    SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);

    std::string cmd_line = "\"" + exe + "\"";
    for (const auto& arg : args) {
        cmd_line += " " + arg;
    }

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = stdin_read;
    si.hStdOutput = stdout_write;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi{};

    BOOL ok = CreateProcessA(
        nullptr, cmd_line.data(),
        nullptr, nullptr,
        TRUE,  // inherit handles
        0, nullptr, nullptr,
        &si, &pi
    );

    // Close child-side pipe ends in parent
    CloseHandle(stdin_read);
    CloseHandle(stdout_write);

    if (!ok) {
        spdlog::error("ProcessHandle::spawn_with_pipes failed: CreateProcess error {}", GetLastError());
        CloseHandle(stdin_write);
        CloseHandle(stdout_read);
        return false;
    }

    CloseHandle(pi.hThread);
    process_handle_ = pi.hProcess;

    // Convert HANDLEs to CRT file descriptors
    child_stdin_write_fd = _open_osfhandle(reinterpret_cast<intptr_t>(stdin_write), 0);
    child_stdout_read_fd = _open_osfhandle(reinterpret_cast<intptr_t>(stdout_read), 0);

    spdlog::info("ProcessHandle: spawned PID {} with pipes ({})", pi.dwProcessId, exe);
    return true;
}

uint64_t ProcessHandle::pid() const {
    if (!process_handle_) return 0;
    return static_cast<uint64_t>(GetProcessId(process_handle_));
}

#else // POSIX

bool ProcessHandle::spawn(const std::string& exe, const std::vector<std::string>& args) {
    reset();

    pid_t child = fork();
    if (child < 0) {
        spdlog::error("ProcessHandle::spawn failed: fork: {}", std::strerror(errno));
        return false;
    }

    if (child == 0) {
        // Child process: exec
        std::vector<const char*> argv;
        argv.push_back(exe.c_str());
        for (const auto& a : args) {
            argv.push_back(a.c_str());
        }
        argv.push_back(nullptr);

        execvp(exe.c_str(), const_cast<char* const*>(argv.data()));
        // If exec returns, it failed
        _exit(127);
    }

    // Parent
    pid_ = child;
    spdlog::info("ProcessHandle: spawned PID {} ({})", pid_, exe);
    return true;
}

bool ProcessHandle::is_alive() const {
    if (pid_ <= 0 || exited_) return false;
    int status = 0;
    pid_t result = waitpid(pid_, &status, WNOHANG);
    if (result == 0) return true;  // still running
    // Exited or error — cache result
    auto* self = const_cast<ProcessHandle*>(this);
    if (result > 0) {
        self->exited_ = true;
        self->exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    return false;
}

int ProcessHandle::wait() {
    if (pid_ <= 0) return exit_code_;
    if (exited_) return exit_code_;

    int status = 0;
    pid_t result = waitpid(pid_, &status, 0);
    if (result > 0) {
        exited_ = true;
        exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    return exit_code_;
}

int ProcessHandle::try_wait() {
    if (pid_ <= 0) return exit_code_;
    if (exited_) return exit_code_;

    int status = 0;
    pid_t result = waitpid(pid_, &status, WNOHANG);
    if (result > 0) {
        exited_ = true;
        exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        return exit_code_;
    }
    return -1;  // still running
}

void ProcessHandle::terminate() {
    if (pid_ > 0 && !exited_) {
        kill(pid_, SIGTERM);
    }
}

bool ProcessHandle::spawn_with_pipes(const std::string& exe, const std::vector<std::string>& args,
                                     int& child_stdin_write_fd, int& child_stdout_read_fd) {
    reset();
    child_stdin_write_fd = -1;
    child_stdout_read_fd = -1;

    int stdin_pipe[2];   // [0]=read (child), [1]=write (parent)
    int stdout_pipe[2];  // [0]=read (parent), [1]=write (child)

    if (pipe(stdin_pipe) != 0) {
        spdlog::error("ProcessHandle::spawn_with_pipes: pipe(stdin): {}", std::strerror(errno));
        return false;
    }
    if (pipe(stdout_pipe) != 0) {
        spdlog::error("ProcessHandle::spawn_with_pipes: pipe(stdout): {}", std::strerror(errno));
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        return false;
    }

    pid_t child = fork();
    if (child < 0) {
        spdlog::error("ProcessHandle::spawn_with_pipes: fork: {}", std::strerror(errno));
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        return false;
    }

    if (child == 0) {
        // Child process
        close(stdin_pipe[1]);   // close parent's write end
        close(stdout_pipe[0]);  // close parent's read end

        dup2(stdin_pipe[0], STDIN_FILENO);
        dup2(stdout_pipe[1], STDOUT_FILENO);

        close(stdin_pipe[0]);
        close(stdout_pipe[1]);

        std::vector<const char*> argv;
        argv.push_back(exe.c_str());
        for (const auto& a : args) {
            argv.push_back(a.c_str());
        }
        argv.push_back(nullptr);

        execvp(exe.c_str(), const_cast<char* const*>(argv.data()));
        _exit(127);
    }

    // Parent process
    close(stdin_pipe[0]);   // close child's read end
    close(stdout_pipe[1]);  // close child's write end

    pid_ = child;
    child_stdin_write_fd = stdin_pipe[1];
    child_stdout_read_fd = stdout_pipe[0];

    spdlog::info("ProcessHandle: spawned PID {} with pipes ({})", pid_, exe);
    return true;
}

uint64_t ProcessHandle::pid() const {
    return static_cast<uint64_t>(pid_);
}

#endif
