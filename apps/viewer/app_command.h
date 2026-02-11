#pragma once

#include <cstdint>
#include <mutex>
#include <variant>
#include <vector>

struct CmdSetSampleRate { double rate; };
struct CmdCycleDecimationMode {};
struct CmdTogglePaused {};
struct CmdToggleVsync {};
struct CmdQuit {};

using AppCommand = std::variant<
    CmdSetSampleRate,
    CmdCycleDecimationMode,
    CmdTogglePaused,
    CmdToggleVsync,
    CmdQuit
>;

class AppCommandQueue {
public:
    void push(AppCommand cmd);
    std::vector<AppCommand> drain();

private:
    std::mutex mutex_;
    std::vector<AppCommand> queue_;
};
