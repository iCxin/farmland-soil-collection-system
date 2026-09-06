// =============================================================================
// system_orchestrator.cpp - 系统编排器实现
// =============================================================================
#include "system_orchestrator.h"
#include "logger.h"
#include "web_server.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace soil {

SystemOrchestrator::SystemOrchestrator(const SystemRunOptions& options)
    : options_(options) {
    boot_time_ = std::chrono::system_clock::now();
}

SystemOrchestrator::~SystemOrchestrator() {
    if (running_) shutdown();
}

// -----------------------------------------------------------------------------
// 系统启动（时序图第1阶段：系统启动初始化）
// -----------------------------------------------------------------------------
bool SystemOrchestrator::startup() {
    {
        SCOPED_CONSOLE_LOCK();
        std::cout << "\n====================================================\n";
        std::cout << "  " << kSystemName << " V" << kSystemVersion << "\n";
        std::cout << "  目标平台: " << TargetPlatformInfo::kCpu << "\n";
        std::cout << "  设备编号: " << kDeviceId << "\n";
        std::cout << "====================================================\n\n";
    }

    // 1. 上电自检（CPU/内存/存储/传感器接口）
    {
        SCOPED_CONSOLE_LOCK();
        std::cout << "[启动] 上电自检: CPU(4核) / 内存(1GB LPDDR4) / 存储(8GB eMMC) / 传感器接口(I2C,SPI,UART,ADC) ... 通过\n";
    }

    // 2. 模块初始化
    logger_ = &Logger::instance();
    logger_->init("logs");

    storage_ = std::make_unique<StorageModule>();
    if (!storage_->initialize("data")) {
        std::cerr << "[启动] 数据持久化模块初始化失败" << std::endl;
        return false;
    }

    sensing_ = std::make_unique<SensingModule>(hardware_, conditioned_queue_, *logger_);
    if (!sensing_->initialize("config/sensors.json")) {
        std::cerr << "[启动] 感知与信号调理模块初始化失败" << std::endl;
        return false;
    }

    fusion_ = std::make_unique<FusionModule>(hardware_, fused_queue_, *storage_, *logger_);
    if (!fusion_->initialize()) {
        std::cerr << "[启动] 智能融合与边缘分析模块初始化失败" << std::endl;
        return false;
    }

    comm_ = std::make_unique<CommModule>(*storage_, *logger_);
    if (!comm_->initialize()) {
        std::cerr << "[启动] 通信与协同模块初始化失败" << std::endl;
        return false;
    }

    autonomy_ = std::make_unique<AutonomyModule>(*logger_, *sensing_, *storage_);
    if (!autonomy_->initialize()) {
        std::cerr << "[启动] 系统自治理与调度模块初始化失败" << std::endl;
        return false;
    }
    autonomy_->registerCoreTasks();

    // 演示场景设置
    if (options_.vehicle_moving) {
        hardware_.setVehicleMoving(true);
        logger_->log("系统", LogLevel::Info, "演示场景: 载具移动中（距离触发采集模式）");
    }
    if (options_.inject_sensor_fault) {
        hardware_.injectSensorFault(0x11);  // 电导率传感器故障
        logger_->log("系统", LogLevel::Info, "演示场景: 已注入电导率传感器故障（观察自恢复流程）");
    }

    // --quiet：控制台静默（日志仍进内存/文件/Web终端）
    if (options_.quiet_console) {
        logger_->setConsoleEcho(false);
    }

    // 3. 启动 Web 监控面板
    if (options_.web_enabled) {
        web_ = std::make_unique<WebServer>(*this, *storage_,
                                           static_cast<uint16_t>(options_.web_port));
        if (web_->start()) {
            web_url_ = "http://127.0.0.1:" + std::to_string(options_.web_port);
            {
                SCOPED_CONSOLE_LOCK();
                std::cout << "[Web] 监控面板已启动: " << web_url_
                          << "  (浏览器打开查看实时仪表盘与终端)" << std::endl;
            }
            logger_->log("Web", LogLevel::Info,
                         "Web 监控面板已启动: " + web_url_ +
                             "（仪表盘 + 内嵌终端 + 趋势图表）");
        } else {
            logger_->log("Web", LogLevel::Warning,
                         "Web 监控面板启动失败（端口 " +
                             std::to_string(options_.web_port) + " 被占用?），系统继续运行");
            web_.reset();
        }
    }

    // 4. 启动六类核心线程
    running_ = true;
    threads_.emplace_back(&SystemOrchestrator::acquisitionThread, this);
    threads_.emplace_back(&SystemOrchestrator::processingThread, this);
    threads_.emplace_back(&SystemOrchestrator::storageForwardThread, this);
    threads_.emplace_back(&SystemOrchestrator::networkListenThread, this);
    threads_.emplace_back(&SystemOrchestrator::scheduledTaskThread, this);
    threads_.emplace_back(&SystemOrchestrator::monitorThread, this);

    startup_ok_ = true;
    logger_->log("系统", LogLevel::Info,
                 "系统启动完成，" + std::to_string(threads_.size()) + " 个核心线程已就绪");
    printBootReport();
    return true;
}

void SystemOrchestrator::printBootReport() {
    SCOPED_CONSOLE_LOCK();
    std::cout << "\n[启动] 传感器自检结果:\n";
    for (const auto& [type, status] : sensing_->allStatuses()) {
        std::cout << "  - " << std::left << std::setw(10) << sensorTypeName(type)
                  << " [" << sensorTypeCode(type) << "] : "
                  << sensorStatusName(status) << "\n";
    }
    if (!web_url_.empty()) {
        std::cout << "[启动] Web 监控面板: " << web_url_ << "\n";
    }
    std::cout << "[启动] 运行时长 " << options_.duration_seconds
              << " 秒，开始主循环...\n\n";
}

// -----------------------------------------------------------------------------
// 1. 数据采集线程：按策略驱动感知模块
// -----------------------------------------------------------------------------
void SystemOrchestrator::acquisitionThread() {
    while (running_) {
        sensing_->runOnce();
        autonomy_->feedWatchdog("acquisition");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

// -----------------------------------------------------------------------------
// 2. 数据处理线程：原始数据 -> 融合分析
// -----------------------------------------------------------------------------
void SystemOrchestrator::processingThread() {
    int cycle = 0;
    while (running_) {
        ConditionedSensorData data;
        // 消费感知模块输出
        while (conditioned_queue_.tryPop(data)) {
            fusion_->submitConditioned(data);
            switch (data.quality_level) {
                case DataQualityLevel::EXCELLENT: ++quality_excellent_; break;
                case DataQualityLevel::GOOD:      ++quality_good_; break;
                case DataQualityLevel::FAIR:      ++quality_fair_; break;
                case DataQualityLevel::POOR:      ++quality_poor_; break;
                default: break;
            }
            {
                std::ostringstream oss;
                oss << std::fixed << std::setprecision(2) << sensorTypeName(data.type)
                    << " = " << data.value
                    << "  质量: " << qualityLevelName(data.quality_level)
                    << " (" << std::setprecision(1) << data.confidence_score << "%)"
                    << (data.is_outlier_rejected ? " [异常值已剔除]" : "");
                logger_->log("采集", LogLevel::Info, oss.str());
            }
            fusion_->checkSensorHealth(data);
        }
        // 周期触发融合（每2秒产生一个融合数据点）
        if (++cycle % 40 == 0) {
            fusion_->runFusionCycle();
        }
        autonomy_->feedWatchdog("processing");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

// -----------------------------------------------------------------------------
// 3. 数据存储与转发线程：事务写入 + MQTT 上传
// -----------------------------------------------------------------------------
void SystemOrchestrator::storageForwardThread() {
    while (running_) {
        FusedRecord record;
        if (fused_queue_.pop(record, std::chrono::milliseconds(200))) {
            // 事务写入数据库（sensor_raw 原始快照 + processed_data 处理结果）
            if (storage_->storeFusedRecord(record, record.contributing_raws)) {
                ++stored_records_;
                {
                    std::ostringstream oss;
                    oss << std::fixed << std::setprecision(2)
                        << "批次 " << record.point.batch_id
                        << " 事务写入: 土壤温度 " << record.point.soil_temp
                        << "°C, 含水率 " << record.point.soil_moisture
                        << "%, pH " << record.point.soil_ph << ", EC "
                        << record.point.soil_ec << "uS/cm"
                        << [&record] {
                               if (record.estimated_fields.empty()) return std::string();
                               std::string s = " (";
                               for (size_t i = 0; i < record.estimated_fields.size(); ++i) {
                                   if (i) s += "/";
                                   s += record.estimated_fields[i] + "估算";
                               }
                               return s + ")";
                           }()
                        << "  [ML] 肥力L" << record.analysis.soil_fertility_level
                        << " 水分胁迫L" << record.analysis.moisture_stress_level
                        << " 盐渍化L" << record.analysis.salinization_risk_level
                        << (record.analysis.anomaly_flag ? " [参数异常]" : "");
                    logger_->log("持久化", LogLevel::Info, oss.str());
                }
            }
            // 通知通信模块上传（MQTT 发布 + 同步状态更新）
            auto result = comm_->uploadPendingData(DataPriority::HIGH);
            uploaded_records_ += result.records_sent;
            if (result.records_sent > 0) {
                std::ostringstream oss;
                oss << "上传 " << result.records_sent
                    << " 条记录经 " << result.interface_used
                    << " (MQTT over TLS)"
                    << (result.retries > 0
                            ? "，含重传 " + std::to_string(result.retries) + " 次"
                            : "");
                logger_->log("通信", LogLevel::Info, oss.str());
            }
        }
        autonomy_->feedWatchdog("storage_forward");
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

// -----------------------------------------------------------------------------
// 4. 网络管理与指令监听线程
// -----------------------------------------------------------------------------
void SystemOrchestrator::networkListenThread() {
    while (running_) {
        comm_->refreshNetworkStatus();
        std::string command;
        if (comm_->pollCloudCommand(command)) {
            // 云端指令转交自治模块执行（时序图第4阶段：指令处理流程）
            logger_->log("系统", LogLevel::Info, "云端指令转交系统自治模块执行");
            comm_->applyConfigurationTransaction(
                {{"sampling_interval_multiplier", "1"}, {"upload_policy", "batch"}});
        }
        autonomy_->feedWatchdog("network_listen");
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

// -----------------------------------------------------------------------------
// 5. 定时任务线程：归档清理、状态心跳、功耗管理
// -----------------------------------------------------------------------------
void SystemOrchestrator::scheduledTaskThread() {
    int tick = 0;
    while (running_) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        ++tick;
        // 每10秒：数据库维护（老化/归档）+ 心跳上报
        if (tick % 10 == 0) {
            storage_->runMaintenance();
            comm_->publishStatusHeartbeat();
        }
        // 每5秒：功耗管理（电池电量线性缓降仿真）
        if (tick % 5 == 0) {
            double battery = 92.0 - static_cast<double>(tick) / 60.0;
            autonomy_->managePower(options_.vehicle_moving, battery);
        }
        autonomy_->feedWatchdog("scheduled_task");
    }
}

// -----------------------------------------------------------------------------
// 6. 主监控线程：看门狗、资源监控、异常检测（最高优先级）
// -----------------------------------------------------------------------------
void SystemOrchestrator::monitorThread() {
    while (running_) {
        std::this_thread::sleep_for(std::chrono::seconds(2));

        // 资源监控与性能分析
        auto metrics = autonomy_->collectSystemMetrics(
            6, static_cast<size_t>(conditioned_queue_.size() + fused_queue_.size()),
            comm_->isOnline() ? 0.55 : 0.0);
        autonomy_->analyzeResources(metrics);

        // 异常检测与自动恢复
        autonomy_->detectAndRecover(metrics, sensing_->allStatuses());

        // 看门狗：检查所有线程心跳，超时告警（实际平台喂硬件看门狗）
        autonomy_->feedWatchdog("monitor");
        auto dead = autonomy_->deadThreads();
        if (!dead.empty()) {
            logger_->log("自治", LogLevel::Error,
                         "看门狗检测到线程心跳超时: " + dead[0] + "，触发恢复");
        }
    }
}

// -----------------------------------------------------------------------------
// 主运行循环
// -----------------------------------------------------------------------------
void SystemOrchestrator::run() {
    auto start = std::chrono::steady_clock::now();
    while (running_) {
        auto elapsed = std::chrono::steady_clock::now() - start;
        if (std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() >=
            options_.duration_seconds) {
            logger_->log("系统", LogLevel::Info, "达到设定运行时长，开始优雅停机");
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    shutdown();
}

// -----------------------------------------------------------------------------
// 优雅停机
// -----------------------------------------------------------------------------
void SystemOrchestrator::shutdown() {
    if (!running_.exchange(false)) return;

    // 停止 Web 面板
    if (web_) {
        web_->stop();
        web_.reset();
    }
    // 排空处理队列
    conditioned_queue_.shutdown();
    fused_queue_.shutdown();
    for (auto& t : threads_) {
        if (t.joinable()) t.join();
    }
    // 最终落盘
    storage_->flush();
    logger_->flushToDisk();
    printFinalReport();
}

void SystemOrchestrator::printFinalReport() {
    auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
                      std::chrono::system_clock::now() - boot_time_).count();
    auto sensing_stats = sensing_->statistics();
    auto storage_stats = storage_->statistics();

    SCOPED_CONSOLE_LOCK();
    std::cout << "\n==================== 系统运行报告 ====================\n";
    std::cout << "  运行时长:           " << uptime << " 秒\n";
    std::cout << "  [感知] 采集样本:    " << sensing_stats.samples_collected
              << " (距离触发 " << sensing_stats.distance_triggered_samples
              << ", 异常值剔除 " << sensing_stats.outliers_rejected
              << ", 读取错误 " << sensing_stats.read_errors << ")\n";
    std::cout << "  [感知] 高频模式切换: " << sensing_stats.high_freq_mode_switches
              << " 次, 自恢复成功: " << sensing_stats.sensors_auto_recovered << " 次\n";
    std::cout << "  [融合] 融合数据点:  " << fusion_->fused_count() << "\n";
    std::cout << "  [存储] 事务提交:    " << storage_stats.transaction_commits
              << " (回滚 " << storage_stats.transaction_rollbacks
              << "), 当前待同步 " << storage_stats.unsynced_count << " 条\n";
    std::cout << "  [存储] 归档块:      " << storage_stats.archived_blocks
              << ", 老化删除 " << storage_stats.aging_deletions << " 条\n";
    std::cout << "  [通信] MQTT上传:    " << uploaded_records_
              << " 条, 分片 " << comm_->fragments_sent_total()
              << ", ACK失败 " << comm_->ack_failures() << "\n";
    std::cout << "  [自治] 异常检出:    " << autonomy_->anomaly_count()
              << ", 恢复执行 " << autonomy_->recoveries_performed()
              << ", 系统健康评分 " << autonomy_->systemHealthScore() << "/100\n";
    std::cout << "  [质量分布] 优秀 " << quality_excellent_
              << " / 良好 " << quality_good_
              << " / 一般 " << quality_fair_
              << " / 差 " << quality_poor_ << "\n";
    std::cout << "  日志: logs/system.log  数据库: data/soil_database.json\n";
    std::cout << "======================================================\n";
}


// -----------------------------------------------------------------------------
// Web 面板支持：状态快照与访问地址
// -----------------------------------------------------------------------------
StatusSnapshot SystemOrchestrator::statusSnapshot() const {
    StatusSnapshot snap;
    snap.device_id = kDeviceId;
    snap.version = kSystemVersion;
    snap.uptime_s = std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::system_clock::now() - boot_time_).count();
    if (!autonomy_) return snap;

    snap.health = autonomy_->systemHealthScore();
    snap.device_state = deviceStateName(autonomy_->deviceState());
    snap.network_online = comm_->isOnline();
    snap.network_interface = comm_->currentInterface();

    for (const auto& [type, status] : sensing_->allStatuses()) {
        snap.sensors.push_back({sensorTypeName(type), sensorTypeCode(type),
                                sensorStatusName(status)});
    }
    const auto& ss = sensing_->statistics();
    snap.sensing_samples = ss.samples_collected;
    snap.sensing_outliers = ss.outliers_rejected;
    snap.sensing_errors = ss.read_errors;
    snap.sensing_distance = ss.distance_triggered_samples;
    snap.sensing_high_freq = ss.high_freq_mode_switches;

    snap.fusion_points = fusion_->fused_count();

    const auto& sto = storage_->statistics();
    snap.storage_commits = sto.transaction_commits;
    snap.storage_unsynced = sto.unsynced_count;
    snap.storage_archived = sto.archived_blocks;
    snap.storage_processed = sto.processed_total;

    snap.comm_uploaded = uploaded_records_;
    snap.comm_fragments = comm_->fragments_sent_total();
    snap.comm_ack_fail = comm_->ack_failures();

    snap.autonomy_anomalies = autonomy_->anomaly_count();
    snap.autonomy_recoveries = autonomy_->recoveries_performed();

    snap.q_excellent = quality_excellent_;
    snap.q_good = quality_good_;
    snap.q_fair = quality_fair_;
    snap.q_poor = quality_poor_;

    if (fusion_->hasLastPoint()) {
        const auto& p = fusion_->lastPoint();
        snap.has_latest = true;
        snap.latest_batch = p.batch_id;
        snap.latest_temp = p.soil_temp;
        snap.latest_moisture = p.soil_moisture;
        snap.latest_ph = p.soil_ph;
        snap.latest_ec = p.soil_ec;
        snap.latest_quality = static_cast<int>(p.data_quality);
    }
    return snap;
}

std::string SystemOrchestrator::webUrl() const { return web_url_; }

}  // namespace soil
