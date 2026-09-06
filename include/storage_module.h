// =============================================================================
// storage_module.h - 模块三：数据组织与持久化
//
// 职责：
//   1. 数据封装与事务写入（sensor_raw + processed_data 原子提交，支持回滚）
//   2. 数据老化与清理（已同步数据优先淘汰的 LRU 策略）
//   3. 自适应压缩与存储优化（差分+变长编码归档、四分区存储管理）
//   4. 数据索引与快速检索（时间索引 / Geohash空间索引 / 特征直方图索引）
//
// 存储引擎：无外部依赖的嵌入式文件数据库（JSON 行存 + 原子替换落盘），
// 含 config / sensor_raw / processed_data / system_events 四张表。
// 部署到目标平台时可等价替换为 SQLite3 引擎。
// =============================================================================
#pragma once

#include "common.h"
#include "data_queue.h"

namespace soil {

struct FusedRecord;

// processed_data 表行结构
struct ProcessedDataRow {
    int id = 0;
    int batch_id = 0;
    double latitude = 0.0;
    double longitude = 0.0;
    double soil_temp = 0.0;
    double soil_moisture = 0.0;
    double soil_ph = 0.0;
    double soil_ec = 0.0;
    int data_quality = 0;
    int64_t timestamp_ms = 0;
    int is_synced = 0;
    std::string geohash;            // 空间索引
};

// sensor_raw 表行结构
struct RawDataRow {
    int id = 0;
    int batch_id = 0;
    std::string sensor_type;
    double raw_value = 0.0;
    int64_t timestamp_ms = 0;
};

// system_events 表行结构
struct SystemEventRow {
    int id = 0;
    std::string level;      // INFO / WARN / ERROR
    std::string module;
    int code = 0;
    std::string message;
    int64_t timestamp_ms = 0;
};

// 存储统计
struct StorageStatistics {
    int processed_total = 0;
    int raw_total = 0;
    int unsynced_count = 0;
    int archived_blocks = 0;
    uint64_t realtime_zone_bytes = 0;   // 实时数据区
    uint64_t archive_zone_bytes = 0;    // 归档数据区（SD卡）
    double last_compression_ratio = 1.0;
    int aging_deletions = 0;
    int transaction_commits = 0;
    int transaction_rollbacks = 0;
};

class StorageModule {
public:
    StorageModule();

    bool initialize(const std::string& data_dir);

    // - 数据封装与事务写入 ----
    // 一个采集批次原子写入：原始快照 -> processed_data（事务保证一致性）
    bool storeFusedRecord(const FusedRecord& record,
                          const std::vector<ConditionedSensorData>& batch_raws);

    // ---- 同步管理（配合通信模块） ----
    std::vector<ProcessedDataRow> fetchUnsyncedBatch(int limit);
    void markSynced(const std::vector<int>& ids);

    // ---- 系统事件 ----
    void logSystemEvent(LogLevel level, const std::string& module,
                        int code, const std::string& message);

    // ---- 配置表（支持远程热更新） ----
    std::string getConfig(const std::string& key, const std::string& fallback = "");
    void setConfig(const std::string& key, const std::string& value);

    // ---- ML 模型参数（OTA 更新通道） ----
    std::vector<double> loadMlModelParams();
    void saveMlModelParams(const std::vector<double>& params);

    // - 数据老化  / 归档 ----
    // 实时区使用率超阈值时触发归档 + LRU清理（由定时任务线程调用）
    void runMaintenance();

    // - 查询接口 ----
    std::vector<ProcessedDataRow> queryByTimeRange(int64_t start_ms, int64_t end_ms,
                                                    int limit = 100) const;
    std::vector<ProcessedDataRow> queryBySpatialPrefix(const std::string& geohash_prefix,
                                                        int limit = 100) const;
    std::vector<ProcessedDataRow> queryByThreshold(const std::string& field,
                                                    double threshold, bool above,
                                                    int limit = 100) const;

    StorageStatistics statistics() const;
    void flush();   // 强制落盘

private:
    // JSON 数据库文件原子替换
    void loadDatabase();
    void commitDatabase();          // 事务提交：tmp 文件 + 原子重命名
    void rollbackTransaction();

    // 差分+变长编码压缩归档
    bool archiveOldestBatch();

    // 索引维护
    static std::string geohashEncode(double lat, double lon, int precision = 8);
    void rebuildHistogramIndex();

    std::string data_dir_;
    std::string db_path_;
    std::string archive_dir_;
    mutable std::mutex db_mutex_;  // 查询接口为 const，需要可锁定

    // 内存表
    std::vector<ProcessedDataRow> processed_table_;
    std::vector<RawDataRow> raw_table_;
    std::vector<SystemEventRow> events_table_;
    std::map<std::string, std::string> config_table_;

    // 事务暂存区
    struct Transaction {
        bool active = false;
        std::vector<ProcessedDataRow> processed_backup;
        std::vector<RawDataRow> raw_backup;
        int next_processed_id = 0;
        int next_raw_id = 0;
    };
    Transaction txn_;
    int next_processed_id_ = 1;
    int next_raw_id_ = 1;
    int next_event_id_ = 1;

    // 特征直方图索引（字段 -> 分箱 -> 计数）
    std::map<std::string, std::map<int, int>> histogram_index_;

    // 存储分区参数
    static constexpr size_t kRealtimeZoneLimitRecords = 500;    // 实时区容量
    static constexpr double kArchiveTriggerRatio = 0.8;         // 80% 触发归档
    static constexpr size_t kArchiveBlockRecords = 1000;        // 归档块大小
    static constexpr int kEventLogLimit = 300;                  // 事件表上限

    StorageStatistics stats_;
    bool initialized_ = false;
};

}  // namespace soil
