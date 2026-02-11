#include "app_command.h"

void AppCommandQueue::push(AppCommand cmd) {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.push_back(std::move(cmd));
}

std::vector<AppCommand> AppCommandQueue::drain() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<AppCommand> result;
    result.swap(queue_);
    return result;
}
