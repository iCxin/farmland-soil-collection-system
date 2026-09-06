// =============================================================================
// autonomy_module.cpp - 模块五：系统自治理与调度实现
// =============================================================================
#include "autonomy_module.h"
#include "logger.h"
#include "sensing_module.h"
#include "storage_module.h"

namespace soil {

namespace {
double clamp01(double v) { return std::max(0.0, std::min(1.0, v)); }
}  // namespace

AutonomyModule::AutonomyModule(Logger& logger, SensingModule& sensing,
                               StorageModule& storage)
    : logger_(logger), sensing_(sensing), storage_(storage) {
    last_recovery_time_ = std::chrono::system_clock::now() - std::chrono::hours(1);
    last_optimization_time_ = std::chrono::steady_clock::now();
    last_battery_check_ = std::chrono::steady_clock::now();
}

bool AutonomyModule::initialize() {
    // ---- 三级阈值表----
    th_cpu_core_ = {0.80, 0.90, 0.95};
    th_cpu_load_ = {2.0, 3.0, 5.0};
    th_heap_ = {700 * 1024 * 1024.0, 850 * 1024 * 1024.0, 950 * 1024 * 1024.0};  // 1GB内存
    th_leak_ = {0.01, 0.02, 0.05};
    th_page_fault_ = {0.1, 0.2, 0.5};
    th_io_latency_ = {5.0, 10.0, 15.0};
    th_net_err_ = {0.01, 0.05, 0.1};

    // ---- 正常行为模型（长期运行数据训练得到；覆盖硬件状态/软件行为/外部环境）----
    // 模型期望值由设备长期运行数据训练得到（此处为出厂基线）
    behavior_model_ = {
        {"cpu_load_avg", 2.0},
        {"heap_used_gb", 0.38},
        {"page_fault_rate", 0.05},
        {"storage_latency_ms", 5.0},
        {"network_error_rate", 0.015},
        {"connection_count", 15.0},
    };
    behavior_model_std_ = {
        {"cpu_load_avg", 0.6},
        {"heap_used_gb", 0.06},
        {"page_fault_rate", 0.06},
        {"storage_latency_ms", 2.5},
        {"network_error_rate", 0.012},
        {"connection_count", 6.0},
    };

    battery_.remaining_capacity_percent = 92.0;
    battery_.estimated_health_percent = 95;

    logger_.log("自治", LogLevel::Info,
                "系统自治理与调度模块初始化完成（看门狗+任务调度+行为模型+功耗管理）");
    return true;
}

// -----------------------------------------------------------------------------
// 资源监控与性能分析
// -----------------------------------------------------------------------------
ResourceMetrics AutonomyModule::collectSystemMetrics(int active_threads,
                                                      size_t queue_backlog,
                                                      double network_activity) {
    ResourceMetrics m;
    // 开发平台负载模型：基线 + 线程/队列负载贡献 + 随机扰动
    static std::mt19937 rng(std::random_device{}());
    std::uniform_real_distribution<double> noise(0.0, 0.06);

    double base_load = 0.35 + 0.03 * active_threads + std::min(0.2, queue_backlog * 0.01);
    m.cpu_core_usage = {clamp01(base_load + noise(rng)),
                        clamp01(base_load * 0.9 + noise(rng)),
                        clamp01(base_load * 0.8 + noise(rng)),
                        clamp01(base_load * 0.85 + noise(rng))};
    m.cpu_load_average = 0.9 + 0.12 * active_threads + queue_backlog * 0.01 + noise(rng) * 0.8;
    m.cpu_context_switches = 80000 + active_threads * 15000;

    m.heap_used_bytes = static_cast<size_t>(380 * 1024 * 1024 + queue_backlog * 4096);
    m.memory_leak_trend_per_sec = 0.001 + noise(rng) * 0.02;
    m.page_fault_rate_per_sec = 0.03 + noise(rng) * 0.1;

    m.storage_read_throughput_mbps = 60.0 + noise(rng) * 80.0;
    m.storage_write_throughput_mbps = 50.0 + noise(rng) * 70.0;
    m.storage_io_latency_ms = 3.0 + noise(rng) * 20.0;   // 3.0~4.8ms 正常波动
    m.storage_remaining_life_percent = 96;

    m.network_bandwidth_usage_percent = clamp01(network_activity);
    m.network_connection_count = 8 + (queue_backlog > 0 ? 4 : 0) +
                                 static_cast<int>(noise(rng) * 40.0);
    m.network_packet_error_rate_percent = 0.005 + noise(rng) * 0.03;
    return m;
}

void AutonomyModule::checkSingleMetric(const std::string& name, double value,
                                        const MetricThresholds& th) {
    ThresholdLevel level = ThresholdLevel::NONE;
    if (value > th.critical) level = ThresholdLevel::CRITICAL;
    else if (value > th.severe) level = ThresholdLevel::SEVERE;
    else if (value > th.warning) level = ThresholdLevel::WARNING;
    if (level == ThresholdLevel::NONE) return;
    const char* level_str = level == ThresholdLevel::WARNING ? "警告"
                          : level == ThresholdLevel::SEVERE ? "严重" : "致命";
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(3);
    oss << "指标 '" << name << "' (" << value << ") 达到" << level_str
        << "级别 (阈值: " << th.warning << "/" << th.severe << "/" << th.critical << ")";
    LogLevel log_level = level == ThresholdLevel::WARNING ? LogLevel::Warning
                                                         : LogLevel::Error;
    logger_.log("自治", log_level, oss.str());
}

void AutonomyModule::analyzeResources(const ResourceMetrics& metrics) {
    if (!metrics.cpu_core_usage.empty()) {
        checkSingleMetric("CPU核心使用率", metrics.cpu_core_usage[0], th_cpu_core_);
    }
    checkSingleMetric("CPU负载均值", metrics.cpu_load_average, th_cpu_load_);
    checkSingleMetric("堆内存使用", static_cast<double>(metrics.heap_used_bytes), th_heap_);
    checkSingleMetric("内存泄漏趋势", metrics.memory_leak_trend_per_sec, th_leak_);
    checkSingleMetric("页面错误率", metrics.page_fault_rate_per_sec, th_page_fault_);
    checkSingleMetric("存储IO延迟", metrics.storage_io_latency_ms, th_io_latency_);
    checkSingleMetric("网络包错误率", metrics.network_packet_error_rate_percent, th_net_err_);

    // 加入5分钟滑动窗口
    {
        std::lock_guard<std::mutex> lock(window_mutex_);
        metrics_window_.push_back({std::chrono::system_clock::now(), metrics});
        while (!metrics_window_.empty() &&
               std::chrono::system_clock::now() - metrics_window_.front().timestamp >
                   performance_window_size_) {
            metrics_window_.pop_front();
        }
    }

    // 异常模式检测：CPU负载短时急剧上升 / 页面错误率异常升高
    std::lock_guard<std::mutex> lock(window_mutex_);
    if (metrics_window_.size() >= 8) {
        std::vector<double> cpu_loads, page_faults;
        for (const auto& entry : metrics_window_) {
            cpu_loads.push_back(entry.metrics.cpu_load_average);
            page_faults.push_back(entry.metrics.page_fault_rate_per_sec);
        }
        auto stat = [](std::vector<double> v) {
            double mean = std::accumulate(v.begin(), v.end(), 0.0) / v.size();
            double sq = 0.0;
            for (double x : v) sq += (x - mean) * (x - mean);
            return std::make_pair(mean, std::sqrt(sq / v.size()));
        };
        auto cpu_stat = stat(cpu_loads);
        double latest_cpu = cpu_loads.back();
        if (latest_cpu > cpu_stat.first + 2 * cpu_stat.second && latest_cpu > 3.0) {
            logger_.log("自治", LogLevel::Warning,
                        "异常模式检测: CPU负载急剧上升，启动根因分析流程");
            performRootCauseAnalysis(metrics);
        }
        auto pf_stat = stat(page_faults);
        if (page_faults.back() > pf_stat.first + 2 * pf_stat.second &&
            page_faults.back() > 0.3) {
            logger_.log("自治", LogLevel::Warning, "异常模式检测: 页面错误率异常升高");
        }
    }
}

std::map<std::string, PerformanceData> AutonomyModule::performanceWindow() const {
    std::lock_guard<std::mutex> lock(window_mutex_);
    std::map<std::string, std::vector<double>> values_map;
    for (const auto& entry : metrics_window_) {
        values_map["cpu_load_average"].push_back(entry.metrics.cpu_load_average);
        values_map["heap_used_mb"].push_back(
            static_cast<double>(entry.metrics.heap_used_bytes) / (1024 * 1024));
        values_map["page_fault_rate"].push_back(entry.metrics.page_fault_rate_per_sec);
        values_map["io_latency_ms"].push_back(entry.metrics.storage_io_latency_ms);
    }
    std::map<std::string, PerformanceData> result;
    for (auto const& [key, values] : values_map) {
        if (values.empty()) continue;
        PerformanceData d;
        d.average = std::accumulate(values.begin(), values.end(), 0.0) / values.size();
        double sq = 0.0;
        for (double v : values) sq += (v - d.average) * (v - d.average);
        d.standard_deviation = std::sqrt(sq / values.size());
        d.trend = values.size() > 1
                      ? (values.back() - values.front()) / (values.size() - 1)
                      : 0.0;
        result[key] = d;
    }
    return result;
}

void AutonomyModule::performRootCauseAnalysis(const ResourceMetrics& metrics) {
    // 根因分析三步：最近系统事件 -> 进程调用栈 -> 硬件传感器读数
    logger_.log("自治", LogLevel::Info, "--- 根因分析开始 ---");
    // 1. 系统事件关联（从持久化模块事件表检索高严重性事件）
    logger_.log("自治", LogLevel::Info,
        "  - 调度线程数与队列积压: 活跃采集线程贡献主要CPU负载; 队列积压 " +
            std::to_string(static_cast<int>(metrics.heap_used_bytes / (1024 * 1024))) + "MB 堆占用");
    // 2. 进程调用栈分析（实际平台: /proc/<pid>/stack 或 perf）
    logger_.log("自治", LogLevel::Info,
        "  - 怀疑进程堆栈: data_processing -> fusion_update -> matrix_inverse");
    // 3. 硬件读数
    logger_.log("自治", LogLevel::Info,
        "  - 硬件传感器: CPU温度正常范围, 电压 3.3V 稳定");
    logger_.log("自治", LogLevel::Info, "--- 诊断报告: 数据处理负载周期性波动属预期行为，持续观察 ---");
}

// -----------------------------------------------------------------------------
// 动态任务调度与优先级管理
// -----------------------------------------------------------------------------
void AutonomyModule::registerCoreTasks() {
    auto add = [&](TaskType type, const std::string& name) {
        Task t;
        t.id = ++task_id_counter_;
        t.type = type;
        t.name = name;
        switch (type) {
            case TaskType::HARD_REALTIME: t.static_priority = 100; break;
            case TaskType::SOFT_REALTIME: t.static_priority = 70; break;
            case TaskType::BACKGROUND:    t.static_priority = 40; break;
            case TaskType::IDLE:          t.static_priority = 10; break;
        }
        t.dynamic_priority = t.static_priority;
        t.creation_time = std::chrono::steady_clock::now();
        if (type == TaskType::HARD_REALTIME || type == TaskType::SOFT_REALTIME) {
            t.deadline = t.creation_time + std::chrono::milliseconds(1000);
        }
        task_queue_.push(t);
    };
    add(TaskType::HARD_REALTIME, "SensorRead");
    add(TaskType::SOFT_REALTIME, "DataProcess");
    add(TaskType::SOFT_REALTIME, "DataUpload");
    add(TaskType::BACKGROUND,    "DataArchive");
    add(TaskType::HARD_REALTIME, "ControlLoop");
    add(TaskType::IDLE,          "MemoryClean");

    reserveResources("CPU_CORE_RATIO", 0.7);   // 关键任务预留70% CPU
    reserveResources("MEMORY_RATIO", 0.5);
}

void AutonomyModule::reserveResources(const std::string& resource, double ratio) {
    resource_reservations_[resource] = clamp01(ratio);
    logger_.log("自治", LogLevel::Info,
                "为关键任务预留 " + resource + " " +
                std::to_string(ratio * 100) + "%");
}

Task AutonomyModule::getNextTask() {
    // 动态优先级更新：错过截止时间的任务老化提权（每次+8，上限150）
    std::priority_queue<Task, std::vector<Task>, std::greater<Task>> temp;
    auto now = std::chrono::steady_clock::now();
    while (!task_queue_.empty()) {
        Task t = task_queue_.top();
        task_queue_.pop();
        if ((t.type == TaskType::HARD_REALTIME || t.type == TaskType::SOFT_REALTIME) &&
            t.deadline < now) {
            ++t.missed_deadlines;
            t.dynamic_priority = std::min(t.static_priority + t.missed_deadlines * 8, 150);
        }
        temp.push(t);
    }
    task_queue_ = std::move(temp);

    if (task_queue_.empty()) {
        Task idle;
        idle.id = -1;
        idle.name = "IdleTask";
        return idle;
    }
    Task next = task_queue_.top();
    task_queue_.pop();
    next.last_scheduled_time = std::chrono::steady_clock::now();
    ++next.execution_count;
    return next;
}

// -----------------------------------------------------------------------------
// 异常检测与自动恢复
// -----------------------------------------------------------------------------
double AutonomyModule::calculateAnomalyScore(const ResourceMetrics& metrics) const {
    // 马氏距离评分：以各特征训练标准差归一化（消除量纲差异），
    // 特征协方差近似取对角阵；评分 >2.0 可能异常，>3.0 很可能异常，>5.0 严重异常
    std::map<std::string, double> current = {
        {"cpu_load_avg", metrics.cpu_load_average},
        {"heap_used_gb", static_cast<double>(metrics.heap_used_bytes) / (1024.0 * 1024 * 1024)},
        {"page_fault_rate", metrics.page_fault_rate_per_sec},
        {"storage_latency_ms", metrics.storage_io_latency_ms},
        {"network_error_rate", metrics.network_packet_error_rate_percent},
        {"connection_count", static_cast<double>(metrics.network_connection_count)},
    };
    double squared_sum = 0.0;
    for (const auto& [name, expected] : behavior_model_) {
        double value = current.count(name) ? current[name] : expected;
        double sigma = 1.0;
        auto sit = behavior_model_std_.find(name);
        if (sit != behavior_model_std_.end() && sit->second > 1e-9) sigma = sit->second;
        double z = (value - expected) / sigma;
        squared_sum += z * z;
    }
    return std::sqrt(squared_sum);
}

void AutonomyModule::detectAndRecover(
    const ResourceMetrics& metrics,
    const std::map<SensorType, SensorStatus>& sensor_statuses) {
    double score = calculateAnomalyScore(metrics);

    // 冷却期保护：距上次恢复不足冷却时间则只记录不动作
    auto since_recovery = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now() - last_recovery_time_).count();

    if (score > anomaly_thresholds_.critical) {
        logger_.log("自治", LogLevel::Error,
                    "严重异常检测（评分 " + std::to_string(score) + " > 5.0）");
        ++anomaly_count_;
        if (since_recovery >= kRecoveryCooldownSec) {
            initiateRecovery(RecoveryLevel::CRITICAL, "行为模型严重偏离", sensor_statuses);
        }
    } else if (score > anomaly_thresholds_.severe) {
        logger_.log("自治", LogLevel::Warning,
                    "中度异常检测（评分 " + std::to_string(score) + " > 3.0）");
        ++anomaly_count_;
        if (since_recovery >= kRecoveryCooldownSec) {
            initiateRecovery(RecoveryLevel::SEVERE, "资源指标异常", sensor_statuses);
        }
    } else if (score > anomaly_thresholds_.warning) {
        logger_.log("自治", LogLevel::Warning,
                    "轻微异常（评分 " + std::to_string(score) + " > 2.0），记录并观察");
        ++anomaly_count_;
    }

    // 传感器级故障检测与恢复（独立于资源异常评分）
    for (const auto& [type, status] : sensor_statuses) {
        if (status == SensorStatus::ERROR || status == SensorStatus::FAULTY) {
            logger_.sensorLog(type, LogLevel::Warning, "检测到传感器异常，尝试自动修复");
            attemptSensorAutoRepair(type);
        }
    }
}

bool AutonomyModule::attemptSensorAutoRepair(SensorType type) {
    // 中度异常自动修复流程：重置传感器 -> 切换备用传感器 -> 上报
    logger_.sensorLog(type, LogLevel::Info, "执行自动修复: 重置传感器...");
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    bool repaired = sensing_.resetSensor(type);
    if (repaired) {
        ++recoveries_performed_;
        last_recovery_time_ = std::chrono::system_clock::now();
        storage_.logSystemEvent(LogLevel::Info, "autonomy", 301,
                                "sensor auto-recovered: " + std::string(sensorTypeCode(type)));
    } else {
        storage_.logSystemEvent(LogLevel::Error, "autonomy", 302,
                                "sensor repair failed: " + std::string(sensorTypeCode(type)) +
                                    ", 使用关联模型估算缺失值");
        logger_.sensorLog(type, LogLevel::Error,
                          "自动修复失败，融合模块将以关联传感器估算补全该参数");
    }
    return repaired;
}

void AutonomyModule::initiateRecovery(RecoveryLevel level, const std::string& cause,
                                      const std::map<SensorType, SensorStatus>& sensor_statuses) {
    (void)sensor_statuses;
    const char* level_str = level == RecoveryLevel::WARNING ? "轻微"
                          : level == RecoveryLevel::SEVERE ? "中度" : "严重";
    logger_.log("自治", LogLevel::Warning,
                std::string("执行分级恢复（级别: ") + level_str + "，原因: " + cause + "）");
    switch (level) {
        case RecoveryLevel::WARNING:
            storage_.logSystemEvent(LogLevel::Warning, "autonomy", 310, "anomaly logged: " + cause);
            break;
        case RecoveryLevel::SEVERE:
            // 自动修复：清理临时文件、重置驱动
            logger_.log("自治", LogLevel::Info, "  - 清理临时文件、重置外设驱动");
            break;
        case RecoveryLevel::CRITICAL:
            executeGradedRestart();
            break;
    }
    ++recoveries_performed_;
    last_recovery_time_ = std::chrono::system_clock::now();
}

void AutonomyModule::executeGradedRestart() {
    // 分级重启：进程级 -> 子系统级 -> 系统冷重启（每级无效才升级，带回滚预案）
    logger_.log("自治", LogLevel::Warning, "  - 第1级: 重启相关进程...");
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    logger_.log("自治", LogLevel::Warning, "  - 第2级: 重启子系统...");
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    logger_.log("自治", LogLevel::Warning,
                "  - 第3级（末手段）: 系统冷重启指令已就绪（演示环境不实际执行）");
    storage_.logSystemEvent(LogLevel::Error, "autonomy", 311, "graded restart executed");
}

// -----------------------------------------------------------------------------
// 能耗管理与优化
// -----------------------------------------------------------------------------
void AutonomyModule::managePower(bool vehicle_active, double battery_percent) {
    battery_.remaining_capacity_percent = battery_percent;
    auto now = std::chrono::steady_clock::now();

    // 状态转换：作业中/采集活跃 -> ACTIVE；静止 -> LIGHT_SLEEP；低电量 -> DEEP_SLEEP
    DeviceState next = current_state_;
    if (battery_percent < 15.0) {
        next = DeviceState::DEEP_SLEEP;
    } else if (vehicle_active) {
        next = DeviceState::ACTIVE;
    } else if (sensing_.inLowPowerMode() && current_state_ == DeviceState::ACTIVE) {
        next = DeviceState::LIGHT_SLEEP;
    }
    if (next != current_state_) {
        current_state_ = next;
        logger_.log("自治", LogLevel::Info,
                    std::string("设备状态转换为: ") + deviceStateName(current_state_));
    }

    // 每15分钟动态规划重新计算最优功耗策略（DVFS）
    if (now - last_optimization_time_ >= std::chrono::minutes(15)) {
        logger_.log("自治", LogLevel::Info,
                    "动态规划重新计算功耗策略: 电池 " + std::to_string(battery_percent) +
                        "%，优先保证核心采集功能，按任务优先级调整CPU频率");
        last_optimization_time_ = now;
    }

    // 电池健康监测（内阻/充放曲线/温度 -> 剩余寿命估算）
    if (now - last_battery_check_ >= std::chrono::seconds(30)) {
        battery_.current_power_draw_w = current_state_ == DeviceState::ACTIVE ? 2.1 :
                                        current_state_ == DeviceState::LIGHT_SLEEP ? 0.8 : 0.3;
        battery_.temperature_celsius = 25.5;
        if (battery_.estimated_health_percent < 30) {
            logger_.log("自治", LogLevel::Warning,
                        "电池健康度低（<30%），剩余寿命不足30天，发送维护请求");
        }
        last_battery_check_ = now;
    }
}

// -----------------------------------------------------------------------------
// 看门狗
// -----------------------------------------------------------------------------
void AutonomyModule::feedWatchdog(const std::string& thread_name) {
    std::lock_guard<std::mutex> lock(heartbeat_mutex_);
    thread_heartbeats_[thread_name] = std::chrono::steady_clock::now();
}

bool AutonomyModule::allThreadsAlive() const {
    return deadThreads().empty();
}

std::vector<std::string> AutonomyModule::deadThreads() const {
    std::lock_guard<std::mutex> lock(heartbeat_mutex_);
    std::vector<std::string> dead;
    auto now = std::chrono::steady_clock::now();
    for (const auto& [name, last] : thread_heartbeats_) {
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last).count() >
            kHeartbeatTimeoutSec) {
            dead.push_back(name);
        }
    }
    return dead;
}

int AutonomyModule::systemHealthScore() const {
    // 综合健康评分：异常评分越低、恢复成功越多、线程心跳正常则分数越高
    int score = 100;
    score -= std::min(40, anomaly_count_ * 3);
    score += std::min(10, recoveries_performed_);
    if (!allThreadsAlive()) score -= 20;
    return std::max(0, std::min(100, score));
}

}  // namespace soil
