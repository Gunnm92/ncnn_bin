#include "utils/logger.hpp"

#include <atomic>
#include <iostream>

namespace logger {
namespace {
std::atomic<int> g_level{static_cast<int>(Level::Warn)};
} // namespace

void set_level(Level level) {
    g_level.store(static_cast<int>(level), std::memory_order_relaxed);
}

Level level() {
    return static_cast<Level>(g_level.load(std::memory_order_relaxed));
}

void info(const std::string& message) {
    if (g_level.load(std::memory_order_relaxed) < static_cast<int>(Level::Info)) {
        return;
    }
    std::clog << "[INFO] " << message << "\n";
}

void warn(const std::string& message) {
    if (g_level.load(std::memory_order_relaxed) < static_cast<int>(Level::Warn)) {
        return;
    }
    std::clog << "[WARN] " << message << "\n";
}

void error(const std::string& message) {
    std::clog << "[ERROR] " << message << "\n";
}
} // namespace logger
