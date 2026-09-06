// =============================================================================
// logger.h - 诊断日志子系统
// 提供线程安全的分级日志：控制台输出 + 内存环形缓冲 + 文件持久化 + Web广播
// =============================================================================
#pragma once

#include "common.h"

#include <deque>
#include <functional>

namespace soil {

class Logger {
public:
    static Logger& instance();

    // 初始化：指定日志文件目录
    void init(const std::string& log_dir);

    // 写入一条日志（线程安全）
    void log(const std::string& module, LogLevel level, const std::string& message);

    // 传感器诊断日志快捷方法
    void sensorLog(SensorType type, LogLevel level, const std::string& message);

    // 设置广播钩子：每条日志产生时回调（供 Web 终端 SSE 实时推送）
    void setBroadcastHook(std::function<void(const DiagnosticLogEntry&)> hook) {
        broadcast_hook_ = std::move(hook);
    }

    // 控制台回显开关（关闭后日志仅进内存/文件/Web，控制台静默）
    void setConsoleEcho(bool enabled) {
        std::lock_guard<std::mutex> lock(mutex_);
        console_echo_ = enabled;
    }

    // 获取最近的诊断日志条目
    std::vector<DiagnosticLogEntry> recentEntries(size_t count) const;

    // 按模块统计错误数（供自治理模块健康评估）
    int errorCount(const std::string& module) const;

    // 将内存日志批量落盘
    void flushToDisk();

    // 运行统计
    int total_count() const { return total_count_; }
    int warning_count() const { return warning_count_; }
    int error_count() const { return error_count_; }

private:
    Logger() = default;
    void writeToFile(const DiagnosticLogEntry& entry);

    mutable std::mutex mutex_;
    std::deque<DiagnosticLogEntry> ring_buffer_;    // 环形缓冲
    size_t ring_capacity_ = 2000;
    std::string log_file_path_;
    std::FILE* log_file_ = nullptr;
    int total_count_ = 0;
    int warning_count_ = 0;
    int error_count_ = 0;
    bool console_echo_ = true;
    std::function<void(const DiagnosticLogEntry&)> broadcast_hook_;
    std::map<std::string, int> module_error_counts_;
};

// 便捷宏
#define LOG_INFO(module, msg)  soil::Logger::instance().log(module, soil::LogLevel::Info, msg)
#define LOG_WARN(module, msg)  soil::Logger::instance().log(module, soil::LogLevel::Warning, msg)
#define LOG_ERROR(module, msg) soil::Logger::instance().log(module, soil::LogLevel::Error, msg)

}  // namespace soil
