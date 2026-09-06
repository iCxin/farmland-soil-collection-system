// =============================================================================
// logger.cpp - 诊断日志子系统实现
// =============================================================================
#include "logger.h"

#include <atomic>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace soil {

std::mutex g_console_mutex;

namespace {
std::atomic<bool> g_logger_initialized{false};

const char* levelTag(LogLevel level) {
    switch (level) {
        case LogLevel::Info:    return "INFO ";
        case LogLevel::Warning: return "WARN ";
        case LogLevel::Error:   return "ERROR";
        default:                return "?????";
    }
}

const char* levelToText(LogLevel level) {
    switch (level) {
        case LogLevel::Info:    return "INFO";
        case LogLevel::Warning: return "WARN";
        case LogLevel::Error:   return "ERROR";
        default:                return "UNKNOWN";
    }
}
}  // namespace

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

void Logger::init(const std::string& log_dir) {
    std::lock_guard<std::mutex> lock(mutex_);
#if defined(_WIN32)
    CreateDirectoryA(log_dir.c_str(), nullptr);
#else
    std::system(("mkdir -p '" + log_dir + "'").c_str());
#endif
    log_file_path_ = log_dir + "/system.log";
#if defined(_WIN32)
    if (fopen_s(&log_file_, log_file_path_.c_str(), "w") != 0) {
        log_file_ = nullptr;
    }
#else
    log_file_ = std::fopen(log_file_path_.c_str(), "w");
#endif
    g_logger_initialized = true;
}

void Logger::log(const std::string& module, LogLevel level, const std::string& message) {
    DiagnosticLogEntry entry;
    entry.timestamp = std::chrono::system_clock::now();
    entry.module = module;
    entry.level = level;
    entry.message = message;

    if (console_echo_) {
        std::string line = "[" + formatTimestamp(entry.timestamp) + "][" +
                           levelTag(level) + "][" + module + "] " + message;
        std::lock_guard<std::mutex> console_lock(g_console_mutex);
        if (level == LogLevel::Error) {
            std::cerr << line << std::endl;
        } else {
            std::cout << line << std::endl;
        }
    }

    // 广播钩子在锁外调用（回调内不得再调用会加 Logger 锁的方法）
    if (broadcast_hook_) broadcast_hook_(entry);

    std::lock_guard<std::mutex> lock(mutex_);
    ring_buffer_.push_back(entry);
    while (ring_buffer_.size() > ring_capacity_) {
        ring_buffer_.pop_front();
    }
    ++total_count_;
    if (level == LogLevel::Warning) ++warning_count_;
    if (level == LogLevel::Error) {
        ++error_count_;
        ++module_error_counts_[module];
    }
    writeToFile(entry);
}

void Logger::sensorLog(SensorType type, LogLevel level, const std::string& message) {
    log(std::string("感知[") + sensorTypeName(type) + "]", level, message);
}

std::vector<DiagnosticLogEntry> Logger::recentEntries(size_t count) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<DiagnosticLogEntry> result;
    size_t n = std::min(count, ring_buffer_.size());
    result.insert(result.end(), ring_buffer_.end() - static_cast<long>(n),
                  ring_buffer_.end());
    return result;
}

int Logger::errorCount(const std::string& module) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = module_error_counts_.find(module);
    return it != module_error_counts_.end() ? it->second : 0;
}

void Logger::flushToDisk() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (log_file_) {
        std::fflush(log_file_);
    }
}

void Logger::writeToFile(const DiagnosticLogEntry& entry) {
    if (!log_file_) return;
    std::string line = "[" + formatTimestampFull(entry.timestamp) + "." +
        std::to_string(toUnixMs(entry.timestamp) % 1000) + "][" +
        levelToText(entry.level) + "][" + entry.module + "] " + entry.message + "\n";
    std::fwrite(line.c_str(), 1, line.size(), log_file_);
}

}  // namespace soil
