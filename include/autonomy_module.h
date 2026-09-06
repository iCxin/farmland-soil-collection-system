// =============================================================================
// autonomy_module.h - 模块五：系统自治理与调度
//
// 职责：
//   1. 资源监控与性能分析（四维监控 / 三级阈值 / 5分钟滑动窗口 / 根因分析）
//   2. 动态任务调度与优先级管理（四级任务 / 优先级老化 / 资源预留）
//   3. 异常检测与自动恢复（正常行为模型 / 马氏距离评分 / 分级恢复）
//   4. 能耗管理与优化（五级设备状态 / DVFS调优 / 预测性功耗 / 电池健康）
// =============================================================================
#pragma once

#include "common.h"

namespace soil {

class Logger;
class SensingModule;
class StorageModule;

// ---------------- 资源监控 ----------------
enum class ThresholdLevel { NONE, WARNING, SEVERE, CRITICAL };
struct MetricThresholds {
    double warning = 0.0;
    double severe = 0.0;
    double critical = 0.0;
};

struct ResourceMetrics {
    // 计算资源
    std::vector<double> cpu_core_usage;    // 各核心使用率 0~1
    double cpu_load_average = 0.0;
    long long cpu_context_switches = 0;
    // 内存资源
    size_t heap_used_bytes = 0;
    double memory_leak_trend_per_sec = 0.0;
    double page_fault_rate_per_sec = 0.0;
    // 存储资源
    double storage_read_throughput_mbps = 0.0;
    double storage_write_throughput_mbps = 0.0;
    double storage_io_latency_ms = 0.0;
    int storage_remaining_life_percent = 100;
    // 网络资源
    double network_bandwidth_usage_percent = 0.0;
    int network_connection_count = 0;
    double network_packet_error_rate_percent = 0.0;
};

struct PerformanceData {
    double average = 0.0;
    double standard_deviation = 0.0;
    double trend = 0.0;
};

// ---------------- 任务调度 ----------------
enum class TaskType { HARD_REALTIME, SOFT_REALTIME, BACKGROUND, IDLE };
inline const char* taskTypeName(TaskType t) {
    switch (t) {
        case TaskType::HARD_REALTIME: return "硬实时";
        case TaskType::SOFT_REALTIME: return "软实时";
        case TaskType::BACKGROUND:    return "后台";
        case TaskType::IDLE:          return "空闲";
        default:                      return "未知";
    }
}

struct Task {
    int id = 0;
    TaskType type = TaskType::IDLE;
    std::string name;
    int static_priority = 0;
    int dynamic_priority = 0;
    std::chrono::steady_clock::time_point creation_time{};
    std::chrono::steady_clock::time_point deadline{};
    int execution_count = 0;
    int missed_deadlines = 0;
    std::chrono::steady_clock::time_point last_scheduled_time{};

    bool operator>(const Task& other) const {
        if (dynamic_priority != other.dynamic_priority) {
            return dynamic_priority < other.dynamic_priority;
        }
        return creation_time > other.creation_time;
    }
};

// ---------------- 能耗管理 ----------------
enum class DeviceState { ACTIVE, LIGHT_SLEEP, DEEP_SLEEP, HIBERNATE, OFF };
inline const char* deviceStateName(DeviceState s) {
    switch (s) {
        case DeviceState::ACTIVE:      return "活跃";
        case DeviceState::LIGHT_SLEEP: return "轻度睡眠";
        case DeviceState::DEEP_SLEEP:  return "深度睡眠";
        case DeviceState::HIBERNATE:   return "冬眠";
        case DeviceState::OFF:         return "关闭";
        default:                       return "未知";
    }
}

struct BatteryInfo {
    double remaining_capacity_percent = 100.0;
    int estimated_health_percent = 100;
    double current_power_draw_w = 2.0;
    double internal_resistance_ohm = 0.08;
    double temperature_celsius = 25.0;
};

// -----------------------------------------------------------------------------
class AutonomyModule {
public:
    AutonomyModule(Logger& logger, SensingModule& sensing, StorageModule& storage);

    bool initialize();

    // 资源采集（实际平台读 /proc/stat /proc/meminfo /proc/net/dev；
    // 开发平台按负载模型仿真，采集线程数与队列积压参与计算）
    ResourceMetrics collectSystemMetrics(int active_threads, size_t queue_backlog,
                                         double network_activity);

    // 阈值检查 + 滑动窗口分析 + 异常模式检测
    void analyzeResources(const ResourceMetrics& metrics);
    std::map<std::string, PerformanceData> performanceWindow() const;

    // 任务调度
    void registerCoreTasks();
    Task getNextTask();
    void reserveResources(const std::string& resource, double ratio);
    int scheduled_task_count() const { return static_cast<int>(task_queue_.size()); }

    // 异常检测与恢复（供监控线程周期调用）
    void detectAndRecover(const ResourceMetrics& metrics,
                          const std::map<SensorType, SensorStatus>& sensor_statuses);
    // 分级恢复接口
    bool attemptSensorAutoRepair(SensorType type);

    // 能耗管理
    void managePower(bool vehicle_active, double battery_percent);
    DeviceState deviceState() const { return current_state_; }
    BatteryInfo batteryInfo() const { return battery_; }

    // 看门狗（软件心跳；实际平台同步喂硬件看门狗 /dev/watchdog）
    void feedWatchdog(const std::string& thread_name);
    bool allThreadsAlive() const;
    std::vector<std::string> deadThreads() const;

    // 健康评分（0~100，供状态上报）
    int systemHealthScore() const;

    // 异常/恢复统计
    int anomaly_count() const { return anomaly_count_; }
    int recoveries_performed() const { return recoveries_performed_; }

private:
    // 内部
    void checkSingleMetric(const std::string& name, double value,
                           const MetricThresholds& thresholds);
    void performRootCauseAnalysis(const ResourceMetrics& metrics);

    // 内部
    double calculateAnomalyScore(const ResourceMetrics& metrics) const;
    enum class RecoveryLevel { WARNING, SEVERE, CRITICAL };
    void initiateRecovery(RecoveryLevel level, const std::string& cause,
                          const std::map<SensorType, SensorStatus>& sensor_statuses);
    void executeGradedRestart();

    Logger& logger_;
    SensingModule& sensing_;
    StorageModule& storage_;

    // 阈值表
    MetricThresholds th_cpu_core_, th_cpu_load_, th_heap_, th_leak_, th_page_fault_,
        th_io_latency_, th_net_err_;
    std::chrono::seconds performance_window_size_{300};  // 5分钟滑动窗口
    struct TimestampedMetrics {
        std::chrono::system_clock::time_point timestamp;
        ResourceMetrics metrics;
    };
    std::deque<TimestampedMetrics> metrics_window_;
    mutable std::mutex window_mutex_;

    // 任务队列
    std::priority_queue<Task, std::vector<Task>, std::greater<Task>> task_queue_;
    std::map<std::string, double> resource_reservations_;
    int task_id_counter_ = 0;

    // 正常行为模型（特征 -> 期望值；由长期运行数据训练）
    std::map<std::string, double> behavior_model_;
    // 各特征的训练标准差（马氏距离归一化，消除量纲差异）
    std::map<std::string, double> behavior_model_std_;
    struct { double warning = 2.0; double severe = 3.0; double critical = 5.0; } anomaly_thresholds_;
    int anomaly_count_ = 0;
    int recoveries_performed_ = 0;
    std::chrono::system_clock::time_point last_recovery_time_;
    static constexpr int kRecoveryCooldownSec = 20;  // 恢复冷却，避免恢复风暴

    // 电源状态
    DeviceState current_state_ = DeviceState::ACTIVE;
    BatteryInfo battery_;
    std::chrono::steady_clock::time_point last_optimization_time_;
    std::chrono::steady_clock::time_point last_battery_check_;

    // 看门狗心跳表
    std::map<std::string, std::chrono::steady_clock::time_point> thread_heartbeats_;
    mutable std::mutex heartbeat_mutex_;
    static constexpr int kHeartbeatTimeoutSec = 15;
};

}  // namespace soil
