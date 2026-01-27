#pragma once

#include <string>

namespace logger {
enum class Level {
    Error = 0,
    Warn = 1,
    Info = 2,
};

// Default: Level::Warn
void set_level(Level level);
Level level();

void info(const std::string& message);
void warn(const std::string& message);
void error(const std::string& message);
} // namespace logger
