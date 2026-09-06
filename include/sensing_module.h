// =============================================================================
// sensing_module.h - 模块一：感知与信号调理
//
// 职责：
//   1. 传感器初始化与自检（厂商ID校验 / 短路校准 / 电极阻抗测量）
//   2. 动态采样调度与触发管理（优先级任务队列 / CAN作业触发 / 距离触发 / 低功耗）
//   3. 原始数据采集与信号调理（滑动平均滤波 / Tukey围栏异常剔除 / 温度补偿）
//   4. 实时校准与质量控制（多项式校准 / pH零点校准 / 置信度评分）
// =============================================================================
#pragma once

#include "common.h"
#include "data_queue.h"
#include "hardware_abstraction.h"

namespace soil {

class Logger;

// 感知模块运行统计
struct SensingStatistics {
    int self_test_passed = 0;
    int self_test_failed = 0;
    int samples_collected = 0;
    int outliers_rejected = 0;
    int high_freq_mode_switches = 0;
    int distance_triggered_samples = 0;
    int read_errors = 0;
    int sensors_auto_recovered = 0;
};

class SensingModule {
public:
    SensingModule(SimulatedHardware& hw,
                  ThreadSafeQueue<ConditionedSensorData>& output_queue,
                  Logger& logger);

    // 加载配置 + 控制器初始化 + 传感器自检 + 建立任务队列
    bool initialize(const std::string& config_file);

    // 单轮调度（由采集线程周期驱动，非阻塞）
    void runOnce();

    // 供自治理模块调用的故障恢复接口（自动修复：重置传感器并重新自检）
    bool resetSensor(SensorType type);

    // 传感器健康状态查询
    SensorStatus sensorStatus(SensorType type) const;
    std::map<SensorType, SensorStatus> allStatuses() const;

    const SensingStatistics& statistics() const { return stats_; }

    // 是否处于低功耗监测模式（静止超10分钟）
    bool inLowPowerMode() const { return low_power_mode_; }
    bool inHighFrequencyMode() const { return job_started_; }

    void shutdown() { running_ = false; }

private:
    // - ----
    bool loadConfigFromJson(const std::string& config_file);
    void configureControllers();
    void selfTestAllSensors();

    // - ----
    void manageSamplingScheduleAndTriggers();
    void handleCanTrigger(const CanSignal& sig);
    void handleLocationAwareTrigger(const GpsData& gps);
    void rebuildTaskQueue();
    int calculateTaskPriority(SensorType type) const;
    int defaultIntervalMs(SensorType type) const;

    // - ----
    ConditionedSensorData collectAndCondition(SensorType type, int sensor_id);

    // - ----
    ConditionedSensorData calibrateAndControlQuality(const ConditionedSensorData& data);

    // 信号调理子步骤
    double movingWindowAverage(SensorType type, double new_value);
    bool tukeyFenceCheck(SensorType type, double value, double window_mean);
    double sensorSpecificCompensation(SensorType type, double value);

    SimulatedHardware& hw_;
    ThreadSafeQueue<ConditionedSensorData>& output_queue_;
    Logger& logger_;

    std::vector<SensorConfig> sensor_config_db_;
    std::map<SensorType, SensorStatus> sensor_statuses_;
    std::map<SensorType, int> type_to_index_;

    // 优先级任务队列（数值越小优先级越高）
    struct ScheduledTask {
        SensorType sensor_type;
        std::chrono::milliseconds interval;
        int priority;
        std::chrono::system_clock::time_point next_execution_time;
        // 优先队列比较：next_execution_time 早者优先，同级时 priority 小者先
        bool operator>(const ScheduledTask& other) const {
            if (next_execution_time != other.next_execution_time) {
                return next_execution_time > other.next_execution_time;
            }
            return priority > other.priority;
        }
    };
    std::priority_queue<ScheduledTask, std::vector<ScheduledTask>, std::greater<ScheduledTask>> task_queue_;
    std::mutex task_queue_mutex_;

    // 触发状态
    bool running_ = true;
    bool job_started_ = false;       // 农机作业中（高频率采集模式）
    bool is_moving_ = false;         // 载具移动中（距离触发模式）
    bool low_power_mode_ = false;    // 静止超时低功耗模式
    double current_speed_mps_ = 0.0;
    double distance_since_last_sample_ = 0.0;  // 距上次采样行驶距离
    std::chrono::system_clock::time_point last_movement_time_;

    // 信号调理状态
    std::map<SensorType, std::deque<double>> filter_windows_;   // 移动窗口（长度10）
    static constexpr size_t kFilterWindowSize = 10;

    SensingStatistics stats_;

    // 演示加速：低功耗静止阈值（实际产品为10分钟）
    std::chrono::minutes stationary_timeout_ = std::chrono::minutes(10);
    double distance_trigger_meters_ = 2.0;   // 每移动2米触发一次
    double movement_speed_threshold_ = 0.5;  // >0.5 m/s 视为移动
};

}  // namespace soil
