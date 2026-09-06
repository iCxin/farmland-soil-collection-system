// =============================================================================
// system_orchestrator.h - 系统编排器：五大功能模块集成与多线程运行时
//
// 运行时创建六类核心线程：
//   1. 主监控线程    - 最高优先级：看门狗喂狗、心跳监测、云端指令分发
//   2. 数据采集线程  - 按策略驱动感知与信号调理模块
//   3. 数据处理线程  - 消费原始数据，执行融合与边缘分析
//   4. 存储转发线程  - 事务写入数据库 + MQTT 上传同步
//   5. 网络监听线程  - 网络状态维护 + 云端指令接收
//   6. 定时任务线程  - 数据归档清理、状态心跳、功耗管理
// =============================================================================
#pragma once

#include "common.h"
#include "data_queue.h"
#include "hardware_abstraction.h"
#include "sensing_module.h"
#include "fusion_module.h"
#include "storage_module.h"
#include "comm_module.h"
#include "autonomy_module.h"

#include <atomic>

namespace soil {

struct SystemRunOptions {
    int duration_seconds = 60;         // 运行时长
    bool vehicle_moving = false;       // 仿真载具移动（距离触发模式）
    bool inject_sensor_fault = false;  // 注入传感器故障（演示自恢复）
    bool quiet_console = false;        // 精简控制台输出（日志仍进Web终端）
    bool web_enabled = true;           // 启用 Web 监控面板
    int web_port = 8080;               // Web 服务端口
};

// Web 面板状态快照（/api/status 数据源）
struct StatusSnapshot {
    std::string device_id;
    std::string version;
    int64_t uptime_s = 0;
    int health = 0;
    std::string device_state;
    bool network_online = false;
    std::string network_interface;
    struct SensorRow { std::string name, code, status; };
    std::vector<SensorRow> sensors;
    int sensing_samples = 0, sensing_outliers = 0, sensing_errors = 0;
    int sensing_distance = 0, sensing_high_freq = 0;
    int fusion_points = 0;
    int storage_commits = 0, storage_unsynced = 0, storage_archived = 0;
    int storage_processed = 0;
    int comm_uploaded = 0, comm_fragments = 0, comm_ack_fail = 0;
    int autonomy_anomalies = 0, autonomy_recoveries = 0;
    int q_excellent = 0, q_good = 0, q_fair = 0, q_poor = 0;
    bool has_latest = false;
    int latest_batch = 0;
    double latest_temp = 0, latest_moisture = 0, latest_ph = 0, latest_ec = 0;
    int latest_quality = 0;
};

class WebServer;  // 前置声明：Web 监控面板

class SystemOrchestrator {
public:
    explicit SystemOrchestrator(const SystemRunOptions& options);
    ~SystemOrchestrator();

    // 系统启动：上电自检 -> 模块初始化 -> 线程启动（时序图第1阶段）
    bool startup();

    // 阻塞运行至时长结束或收到停止信号
    void run();

    // 信号安全停止（供 SIGINT/SIGTERM 处理器调用）
    void requestStop() { running_ = false; }

    // 优雅停机：停采集 -> 排空队列 -> 落盘 -> 汇总报告
    void shutdown();

    // Web 面板状态快照（供 WebServer 线程安全读取）
    StatusSnapshot statusSnapshot() const;

    // Web 面板访问地址（未启用时为空）
    std::string webUrl() const;

private:
    // 各线程主循环
    void acquisitionThread();      // 数据采集线程
    void processingThread();       // 数据处理线程
    void storageForwardThread();   // 数据存储与转发线程
    void networkListenThread();    // 网络管理与指令监听线程
    void scheduledTaskThread();    // 定时任务线程
    void monitorThread();          // 主监控线程

    void printBootReport();
    void printFinalReport();

    SystemRunOptions options_;

    // 模块
    SimulatedHardware hardware_;
    Logger* logger_ = nullptr;   // 单例，非所有权
    std::unique_ptr<StorageModule> storage_;
    std::unique_ptr<SensingModule> sensing_;
    ThreadSafeQueue<ConditionedSensorData> conditioned_queue_;   // 采集->处理
    ThreadSafeQueue<FusedRecord> fused_queue_;                    // 处理->存储
    std::unique_ptr<FusionModule> fusion_;
    std::unique_ptr<CommModule> comm_;
    std::unique_ptr<AutonomyModule> autonomy_;
    std::unique_ptr<WebServer> web_;
    std::string web_url_;

    // 线程
    std::vector<std::thread> threads_;
    std::atomic<bool> running_{false};
    std::atomic<bool> startup_ok_{false};

    // 运行统计
    std::atomic<int> stored_records_{0};
    std::atomic<int> uploaded_records_{0};
    std::atomic<int> quality_excellent_{0};
    std::atomic<int> quality_good_{0};
    std::atomic<int> quality_fair_{0};
    std::atomic<int> quality_poor_{0};
    std::atomic<double> last_upload_rate_{0.0};
    std::chrono::system_clock::time_point boot_time_;
};

}  // namespace soil
