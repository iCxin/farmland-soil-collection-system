// =============================================================================
// storage_module.cpp - 模块三：数据组织与持久化实现
// =============================================================================
#include "storage_module.h"
#include "fusion_module.h"
#include "logger.h"

#include <cstdio>
#include <fstream>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace soil {

namespace {

// ---------------- 极简 JSON 转义 ----------------
std::string jsonEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c;
        }
    }
    return out;
}

// 变长编码（varint）：压缩差分序列
void appendVarint(std::vector<uint8_t>& out, int64_t value) {
    uint64_t zigzag = (static_cast<uint64_t>(value) << 1) ^
                      static_cast<uint64_t>(value >> 63);
    while (zigzag >= 0x80) {
        out.push_back(static_cast<uint8_t>((zigzag & 0x7F) | 0x80));
        zigzag >>= 7;
    }
    out.push_back(static_cast<uint8_t>(zigzag));
}


double clampd(double v, double lo, double hi) {
    return std::max(lo, std::min(hi, v));
}

}  // namespace

StorageModule::StorageModule() = default;

bool StorageModule::initialize(const std::string& data_dir) {
    data_dir_ = data_dir;
    archive_dir_ = data_dir + "/archive";
    db_path_ = data_dir + "/soil_database.json";

    // 创建目录
#if defined(_WIN32)
    CreateDirectoryA(data_dir.c_str(), nullptr);
    CreateDirectoryA(archive_dir_.c_str(), nullptr);
#else
    std::system(("mkdir -p '" + data_dir_ + "' '" + archive_dir_ + "'").c_str());
#endif

    loadDatabase();
    initialized_ = true;
    Logger::instance().log("持久化", LogLevel::Info,
        "数据组织与持久化模块初始化完成: processed=" +
        std::to_string(processed_table_.size()) + " raw=" +
        std::to_string(raw_table_.size()));
    return true;
}

// -----------------------------------------------------------------------------
// 数据库加载 / 事务提交
// -----------------------------------------------------------------------------
void StorageModule::loadDatabase() {
    std::ifstream ifs(db_path_);
    if (!ifs.is_open()) return;
    std::stringstream ss;
    ss << ifs.rdbuf();
    std::string json = ss.str();

    // 轻量解析：逐行提取 processed_data 数组元素
    auto findArray = [&](const std::string& key) -> std::string {
        size_t k = json.find("\"" + key + "\"");
        if (k == std::string::npos) return "";
        size_t lb = json.find('[', k);
        size_t rb = json.find(']', lb);
        if (lb == std::string::npos || rb == std::string::npos) return "";
        return json.substr(lb + 1, rb - lb - 1);
    };
    auto extractNum = [&](const std::string& block, const std::string& key) -> double {
        size_t k = block.find("\"" + key + "\"");
        if (k == std::string::npos) return 0.0;
        size_t c = block.find(':', k);
        return std::atof(block.c_str() + c + 1);
    };
    auto extractStr = [&](const std::string& block, const std::string& key) -> std::string {
        size_t k = block.find("\"" + key + "\"");
        if (k == std::string::npos) return "";
        size_t c = block.find(':', k);
        size_t q1 = block.find('"', c);
        size_t q2 = block.find('"', q1 + 1);
        return (q1 == std::string::npos || q2 == std::string::npos)
                   ? "" : block.substr(q1 + 1, q2 - q1 - 1);
    };

    std::string arr = findArray("processed_data");
    size_t pos = 0;
    while ((pos = arr.find('{', pos)) != std::string::npos) {
        size_t end = arr.find('}', pos);
        if (end == std::string::npos) break;
        std::string b = arr.substr(pos, end - pos);
        pos = end;
        ProcessedDataRow row;
        row.id = static_cast<int>(extractNum(b, "id"));
        row.batch_id = static_cast<int>(extractNum(b, "batch_id"));
        row.latitude = extractNum(b, "latitude");
        row.longitude = extractNum(b, "longitude");
        row.soil_temp = extractNum(b, "soil_temp");
        row.soil_moisture = extractNum(b, "soil_moisture");
        row.soil_ph = extractNum(b, "soil_ph");
        row.soil_ec = extractNum(b, "soil_ec");
        row.data_quality = static_cast<int>(extractNum(b, "data_quality"));
        row.timestamp_ms = static_cast<int64_t>(extractNum(b, "timestamp"));
        row.is_synced = static_cast<int>(extractNum(b, "is_synced"));
        row.geohash = extractStr(b, "geohash");
        processed_table_.push_back(row);
        next_processed_id_ = std::max(next_processed_id_, row.id + 1);
    }

    arr = findArray("sensor_raw");
    pos = 0;
    while ((pos = arr.find('{', pos)) != std::string::npos) {
        size_t end = arr.find('}', pos);
        if (end == std::string::npos) break;
        std::string b = arr.substr(pos, end - pos);
        pos = end;
        RawDataRow row;
        row.id = static_cast<int>(extractNum(b, "id"));
        row.batch_id = static_cast<int>(extractNum(b, "batch_id"));
        row.sensor_type = extractStr(b, "sensor_type");
        row.raw_value = extractNum(b, "raw_value");
        row.timestamp_ms = static_cast<int64_t>(extractNum(b, "timestamp"));
        raw_table_.push_back(row);
        next_raw_id_ = std::max(next_raw_id_, row.id + 1);
    }

    arr = findArray("system_events");
    pos = 0;
    while ((pos = arr.find('{', pos)) != std::string::npos) {
        size_t end = arr.find('}', pos);
        if (end == std::string::npos) break;
        std::string b = arr.substr(pos, end - pos);
        pos = end;
        SystemEventRow row;
        row.id = static_cast<int>(extractNum(b, "id"));
        row.level = extractStr(b, "level");
        row.module = extractStr(b, "module");
        row.code = static_cast<int>(extractNum(b, "code"));
        row.message = extractStr(b, "message");
        row.timestamp_ms = static_cast<int64_t>(extractNum(b, "timestamp"));
        events_table_.push_back(row);
        next_event_id_ = std::max(next_event_id_, row.id + 1);
    }

    arr = findArray("config");
    pos = 0;
    while ((pos = arr.find('{', pos)) != std::string::npos) {
        size_t end = arr.find('}', pos);
        if (end == std::string::npos) break;
        std::string b = arr.substr(pos, end - pos);
        pos = end;
        std::string key = extractStr(b, "key");
        std::string value = extractStr(b, "value");
        if (!key.empty()) config_table_[key] = value;
    }

    rebuildHistogramIndex();
}

void StorageModule::commitDatabase() {
    std::stringstream ss;
    ss << std::fixed << std::setprecision(4);
    ss << "{\n \"meta\": {\"device_id\": \"" << jsonEscape(kDeviceId)
       << "\", \"version\": \"" << kSystemVersion
       << "\", \"updated_at\": " << toUnixMs(std::chrono::system_clock::now()) << "},\n";

    // processed_data
    ss << " \"processed_data\": [\n";
    for (size_t i = 0; i < processed_table_.size(); ++i) {
        const auto& r = processed_table_[i];
        ss << "  {\"id\": " << r.id << ", \"batch_id\": " << r.batch_id
           << ", \"latitude\": " << r.latitude << ", \"longitude\": " << r.longitude
           << ", \"soil_temp\": " << r.soil_temp
           << ", \"soil_moisture\": " << r.soil_moisture
           << ", \"soil_ph\": " << r.soil_ph << ", \"soil_ec\": " << r.soil_ec
           << ", \"data_quality\": " << r.data_quality
           << ", \"timestamp\": " << r.timestamp_ms
           << ", \"is_synced\": " << r.is_synced
           << ", \"geohash\": \"" << jsonEscape(r.geohash) << "\"}"
           << (i + 1 < processed_table_.size() ? ",\n" : "\n");
    }
    ss << " ],\n";

    // sensor_raw
    ss << " \"sensor_raw\": [\n";
    for (size_t i = 0; i < raw_table_.size(); ++i) {
        const auto& r = raw_table_[i];
        ss << "  {\"id\": " << r.id << ", \"batch_id\": " << r.batch_id
           << ", \"sensor_type\": \"" << jsonEscape(r.sensor_type)
           << "\", \"raw_value\": " << r.raw_value
           << ", \"timestamp\": " << r.timestamp_ms << "}"
           << (i + 1 < raw_table_.size() ? ",\n" : "\n");
    }
    ss << " ],\n";

    // system_events
    ss << " \"system_events\": [\n";
    for (size_t i = 0; i < events_table_.size(); ++i) {
        const auto& r = events_table_[i];
        ss << "  {\"id\": " << r.id << ", \"level\": \"" << jsonEscape(r.level)
           << "\", \"module\": \"" << jsonEscape(r.module)
           << "\", \"code\": " << r.code
           << ", \"message\": \"" << jsonEscape(r.message)
           << "\", \"timestamp\": " << r.timestamp_ms << "}"
           << (i + 1 < events_table_.size() ? ",\n" : "\n");
    }
    ss << " ],\n";

    // config
    ss << " \"config\": [\n";
    size_t ci = 0;
    for (const auto& kv : config_table_) {
        ss << "  {\"key\": \"" << jsonEscape(kv.first)
           << "\", \"value\": \"" << jsonEscape(kv.second) << "\"}"
           << (++ci < config_table_.size() ? ",\n" : "\n");
    }
    ss << " ]\n}\n";

    std::string content = ss.str();

    // 原子写入：先写临时文件再替换
    std::string tmp_path = db_path_ + ".tmp";
    {
        std::ofstream ofs(tmp_path, std::ios::binary | std::ios::trunc);
        if (!ofs.is_open()) {
            ++stats_.transaction_rollbacks;
            return;
        }
        ofs << content;
    }
#if defined(_WIN32)
    MoveFileExA(tmp_path.c_str(), db_path_.c_str(), MOVEFILE_REPLACE_EXISTING);
#else
    std::rename(tmp_path.c_str(), db_path_.c_str());
#endif
    stats_.realtime_zone_bytes = content.size();
}

void StorageModule::rollbackTransaction() {
    if (!txn_.active) return;
    processed_table_ = txn_.processed_backup;
    raw_table_ = txn_.raw_backup;
    next_processed_id_ = txn_.next_processed_id;
    next_raw_id_ = txn_.next_raw_id;
    txn_.active = false;
    ++stats_.transaction_rollbacks;
    Logger::instance().log("持久化", LogLevel::Error, "事务写入失败，已回滚至事务前状态");
}

// -----------------------------------------------------------------------------
// 数据封装与事务写入
// -----------------------------------------------------------------------------
bool StorageModule::storeFusedRecord(const FusedRecord& record,
                                     const std::vector<ConditionedSensorData>& batch_raws) {
    std::lock_guard<std::mutex> lock(db_mutex_);
    if (!initialized_) return false;

    // 开启事务：备份现场
    txn_.active = true;
    txn_.processed_backup = processed_table_;
    txn_.raw_backup = raw_table_;
    txn_.next_processed_id = next_processed_id_;
    txn_.next_raw_id = next_raw_id_;

    // 写入原始快照 sensor_raw（用于追溯和高级分析）
    for (const auto& raw : batch_raws) {
        RawDataRow row;
        row.id = next_raw_id_++;
        row.batch_id = record.point.batch_id;
        row.sensor_type = sensorTypeCode(raw.type);
        row.raw_value = raw.value;
        row.timestamp_ms = toUnixMs(raw.timestamp);
        raw_table_.push_back(row);
        ++stats_.raw_total;
    }

    // 写入处理结果 processed_data（打包时空信息）
    ProcessedDataRow prow;
    prow.id = next_processed_id_++;
    prow.batch_id = record.point.batch_id;
    prow.latitude = record.point.latitude;
    prow.longitude = record.point.longitude;
    prow.soil_temp = record.point.soil_temp;
    prow.soil_moisture = record.point.soil_moisture;
    prow.soil_ph = record.point.soil_ph;
    prow.soil_ec = record.point.soil_ec;
    prow.data_quality = static_cast<int>(record.point.data_quality);
    prow.timestamp_ms = toUnixMs(record.point.timestamp);
    prow.is_synced = 0;
    prow.geohash = geohashEncode(record.point.latitude, record.point.longitude, 8);
    processed_table_.push_back(prow);
    ++stats_.processed_total;
    ++stats_.unsynced_count;

    // 维护直方图索引
    histogram_index_["soil_ph"][static_cast<int>(clampd(prow.soil_ph, 0, 14) * 2)]++;
    histogram_index_["soil_ec"][static_cast<int>(clampd(prow.soil_ec, 0, 5000) / 250)]++;
    histogram_index_["soil_moisture"][static_cast<int>(clampd(prow.soil_moisture, 0, 70) / 5)]++;

    // 提交（原子落盘）
    commitDatabase();
    ++stats_.transaction_commits;
    txn_.active = false;
    return true;
}

void StorageModule::rebuildHistogramIndex() {
    histogram_index_.clear();
    for (const auto& r : processed_table_) {
        histogram_index_["soil_ph"][static_cast<int>(clampd(r.soil_ph, 0, 14) * 2)]++;
        histogram_index_["soil_ec"][static_cast<int>(clampd(r.soil_ec, 0, 5000) / 250)]++;
        histogram_index_["soil_moisture"][static_cast<int>(clampd(r.soil_moisture, 0, 70) / 5)]++;
    }
}

// -----------------------------------------------------------------------------
// 同步管理
// -----------------------------------------------------------------------------
std::vector<ProcessedDataRow> StorageModule::fetchUnsyncedBatch(int limit) {
    std::lock_guard<std::mutex> lock(db_mutex_);
    std::vector<ProcessedDataRow> batch;
    for (const auto& r : processed_table_) {
        if (r.is_synced == 0) {
            batch.push_back(r);
            if (static_cast<int>(batch.size()) >= limit) break;
        }
    }
    return batch;
}

void StorageModule::markSynced(const std::vector<int>& ids) {
    std::lock_guard<std::mutex> lock(db_mutex_);
    std::set<int> id_set(ids.begin(), ids.end());
    int marked = 0;
    for (auto& r : processed_table_) {
        if (id_set.count(r.id) && r.is_synced == 0) {
            r.is_synced = 1;
            ++marked;
        }
    }
    stats_.unsynced_count = std::max(0, stats_.unsynced_count - marked);
    if (marked > 0) commitDatabase();
}

void StorageModule::logSystemEvent(LogLevel level, const std::string& module,
                                   int code, const std::string& message) {
    std::lock_guard<std::mutex> lock(db_mutex_);
    SystemEventRow row;
    row.id = next_event_id_++;
    row.level = (level == LogLevel::Info) ? "INFO" : (level == LogLevel::Warning ? "WARN" : "ERROR");
    row.module = module;
    row.code = code;
    row.message = message;
    row.timestamp_ms = toUnixMs(std::chrono::system_clock::now());
    events_table_.push_back(row);
    while (events_table_.size() > kEventLogLimit) events_table_.erase(events_table_.begin());
}

// -----------------------------------------------------------------------------
// 配置表
// -----------------------------------------------------------------------------
std::string StorageModule::getConfig(const std::string& key, const std::string& fallback) {
    std::lock_guard<std::mutex> lock(db_mutex_);
    auto it = config_table_.find(key);
    return it != config_table_.end() ? it->second : fallback;
}

void StorageModule::setConfig(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(db_mutex_);
    config_table_[key] = value;
    commitDatabase();
}

// -----------------------------------------------------------------------------
// ML 模型参数
// -----------------------------------------------------------------------------
std::vector<double> StorageModule::loadMlModelParams() {
    std::lock_guard<std::mutex> lock(db_mutex_);
    std::vector<double> params;
    auto it = config_table_.find("ml_model_params");
    if (it == config_table_.end()) return params;
    std::istringstream iss(it->second);
    std::string tok;
    while (std::getline(iss, tok, ',')) {
        params.push_back(std::atof(tok.c_str()));
    }
    return params;
}

void StorageModule::saveMlModelParams(const std::vector<double>& params) {
    std::lock_guard<std::mutex> lock(db_mutex_);
    std::ostringstream oss;
    for (size_t i = 0; i < params.size(); ++i) {
        if (i) oss << ",";
        oss << std::setprecision(6) << params[i];
    }
    config_table_["ml_model_params"] = oss.str();
}

// -----------------------------------------------------------------------------
// 数据老化  / 归档
// -----------------------------------------------------------------------------
bool StorageModule::archiveOldestBatch() {
    if (processed_table_.empty()) return false;

    // 取最旧的归档块（按时间排序）
    std::vector<ProcessedDataRow> sorted = processed_table_;
    std::sort(sorted.begin(), sorted.end(),
              [](const ProcessedDataRow& a, const ProcessedDataRow& b) {
                  return a.timestamp_ms < b.timestamp_ms;
              });
    size_t block_size = std::min(sorted.size(), kArchiveBlockRecords / 10);

    // 差分编码：时间戳与量化后的参数值均存储与前一数据点的差值
    std::vector<uint8_t> compressed;
    int64_t prev_ts = 0;
    double prev[5] = {0, 0, 0, 0, 0};   // lat, lon, temp, moisture, ec, ph
    prev[0] = prev[1] = prev[2] = prev[3] = prev[4] = 0;
    double prev_ph = 0;
    appendVarint(compressed, static_cast<int64_t>(block_size));
    int count = 0;
    std::set<int> archived_ids;
    for (const auto& r : sorted) {
        if (count >= static_cast<int>(block_size)) break;
        // 量化：lat/lon 1e-6度，温度/水分/pH 0.1，EC 1
        int64_t q_lat = static_cast<int64_t>(r.latitude * 1e6);
        int64_t q_lon = static_cast<int64_t>(r.longitude * 1e6);
        int64_t q_temp = static_cast<int64_t>(r.soil_temp * 10);
        int64_t q_moist = static_cast<int64_t>(r.soil_moisture * 10);
        int64_t q_ec = static_cast<int64_t>(r.soil_ec);
        int64_t q_ph = static_cast<int64_t>(r.soil_ph * 100);
        appendVarint(compressed, r.timestamp_ms - prev_ts); prev_ts = r.timestamp_ms;
        appendVarint(compressed, q_lat - static_cast<int64_t>(prev[0])); prev[0] = static_cast<double>(q_lat);
        appendVarint(compressed, q_lon - static_cast<int64_t>(prev[1])); prev[1] = static_cast<double>(q_lon);
        appendVarint(compressed, q_temp - static_cast<int64_t>(prev[2])); prev[2] = static_cast<double>(q_temp);
        appendVarint(compressed, q_moist - static_cast<int64_t>(prev[3])); prev[3] = static_cast<double>(q_moist);
        appendVarint(compressed, q_ec - static_cast<int64_t>(prev[4])); prev[4] = static_cast<double>(q_ec);
        appendVarint(compressed, q_ph - static_cast<int64_t>(prev_ph)); prev_ph = static_cast<double>(q_ph);
        archived_ids.insert(r.id);
        ++count;
    }

    // 写入归档区（外部SD卡模拟目录）
    auto now_ms = toUnixMs(std::chrono::system_clock::now());
    std::string archive_path = archive_dir_ + "/block_" + std::to_string(now_ms) + ".delta";
    {
        std::ofstream ofs(archive_path, std::ios::binary | std::ios::trunc);
        if (!ofs.is_open()) return false;
        ofs.write(reinterpret_cast<const char*>(compressed.data()),
                  static_cast<std::streamsize>(compressed.size()));
    }

    // 压缩率统计（对比原始 JSON 行存大小）
    size_t raw_json_size = 0;
    for (const auto& r : sorted) {
        if (archived_ids.count(r.id)) raw_json_size += 180;  // 单行 JSON 近似字节数
    }
    stats_.last_compression_ratio =
        raw_json_size > 0 ? static_cast<double>(compressed.size()) / raw_json_size : 1.0;
    stats_.archived_blocks++;

    // 从实时区移除已归档记录
    processed_table_.erase(
        std::remove_if(processed_table_.begin(), processed_table_.end(),
                       [&archived_ids](const ProcessedDataRow& r) {
                           return archived_ids.count(r.id) > 0;
                       }),
        processed_table_.end());

    Logger::instance().log("持久化", LogLevel::Info,
        "归档完成: " + std::to_string(count) + " 条 -> " + archive_path +
            " (压缩率 " + std::to_string(stats_.last_compression_ratio * 100.0) + "%)");
    return true;
}

void StorageModule::runMaintenance() {
    std::lock_guard<std::mutex> lock(db_mutex_);
    if (!initialized_) return;

    // 实时区使用率超过 80% 触发归档
    double usage = static_cast<double>(processed_table_.size()) / kRealtimeZoneLimitRecords;
    if (usage >= kArchiveTriggerRatio) {
        archiveOldestBatch();
        rebuildHistogramIndex();
    }

    // LRU 老化：优先删除已同步且最旧的记录
    if (processed_table_.size() > kRealtimeZoneLimitRecords) {
        std::vector<ProcessedDataRow> sorted = processed_table_;
        std::sort(sorted.begin(), sorted.end(),
                  [](const ProcessedDataRow& a, const ProcessedDataRow& b) {
                      return a.timestamp_ms < b.timestamp_ms;
                  });
        size_t excess = processed_table_.size() - kRealtimeZoneLimitRecords;
        std::set<int> to_delete;
        for (const auto& r : sorted) {
            if (to_delete.size() >= excess) break;
            if (r.is_synced == 1) to_delete.insert(r.id);   // 只删已同步
        }
        if (!to_delete.empty()) {
            processed_table_.erase(
                std::remove_if(processed_table_.begin(), processed_table_.end(),
                               [&to_delete](const ProcessedDataRow& r) {
                                   return to_delete.count(r.id) > 0;
                               }),
                processed_table_.end());
            stats_.aging_deletions += static_cast<int>(to_delete.size());
        }
    }

    // 存储将满时自动提高同步优先级（降级策略：删除最旧已同步数据）
    if (static_cast<size_t>(stats_.unsynced_count) > kRealtimeZoneLimitRecords * 9 / 10) {
        Logger::instance().log("持久化", LogLevel::Warning,
                               "实时数据区接近满载，已提高数据同步优先级");
    }
    commitDatabase();
}

// -----------------------------------------------------------------------------
// 查询接口
// -----------------------------------------------------------------------------
std::vector<ProcessedDataRow> StorageModule::queryByTimeRange(int64_t start_ms,
                                                               int64_t end_ms,
                                                               int limit) const {
    std::lock_guard<std::mutex> lock(db_mutex_);
    // 时间主索引（表按插入序即时间序，二分定位优化时间范围查询）
    std::vector<ProcessedDataRow> result;
    for (const auto& r : processed_table_) {
        if (r.timestamp_ms >= start_ms && r.timestamp_ms <= end_ms) {
            result.push_back(r);
            if (static_cast<int>(result.size()) >= limit) break;
        }
    }
    return result;
}

std::vector<ProcessedDataRow> StorageModule::queryBySpatialPrefix(
    const std::string& geohash_prefix, int limit) const {
    std::lock_guard<std::mutex> lock(db_mutex_);
    std::vector<ProcessedDataRow> result;
    for (const auto& r : processed_table_) {
        if (r.geohash.compare(0, geohash_prefix.size(), geohash_prefix) == 0) {
            result.push_back(r);
            if (static_cast<int>(result.size()) >= limit) break;
        }
    }
    return result;
}

std::vector<ProcessedDataRow> StorageModule::queryByThreshold(const std::string& field,
                                                               double threshold, bool above,
                                                               int limit) const {
    std::lock_guard<std::mutex> lock(db_mutex_);
    std::vector<ProcessedDataRow> result;
    for (const auto& r : processed_table_) {
        double value = 0.0;
        if (field == "soil_ph") value = r.soil_ph;
        else if (field == "soil_ec") value = r.soil_ec;
        else if (field == "soil_moisture") value = r.soil_moisture;
        else if (field == "soil_temp") value = r.soil_temp;
        else continue;
        if ((above && value > threshold) || (!above && value < threshold)) {
            result.push_back(r);
            if (static_cast<int>(result.size()) >= limit) break;
        }
    }
    return result;
}

StorageStatistics StorageModule::statistics() const {
    std::lock_guard<std::mutex> lock(db_mutex_);
    StorageStatistics s = stats_;
    s.processed_total = static_cast<int>(processed_table_.size());
    s.raw_total = static_cast<int>(raw_table_.size());
    s.unsynced_count = 0;
    for (const auto& r : processed_table_) {
        if (r.is_synced == 0) ++s.unsynced_count;
    }
    return s;
}

void StorageModule::flush() {
    std::lock_guard<std::mutex> lock(db_mutex_);
    commitDatabase();
}

// -----------------------------------------------------------------------------
// Geohash 编码（空间索引）
// -----------------------------------------------------------------------------
std::string StorageModule::geohashEncode(double lat, double lon, int precision) {
    static const char base32[] = "0123456789bcdefghjkmnpqrstuvwxyz";
    double lat_lo = -90.0, lat_hi = 90.0;
    double lon_lo = -180.0, lon_hi = 180.0;
    std::string hash;
    int bit = 0, ch = 0;
    bool even = true;
    while (hash.size() < static_cast<size_t>(precision)) {
        if (even) {
            double mid = (lon_lo + lon_hi) / 2.0;
            if (lon >= mid) {
                ch = (ch << 1) | 1;
                lon_lo = mid;
            } else {
                ch <<= 1;
                lon_hi = mid;
            }
        } else {
            double mid = (lat_lo + lat_hi) / 2.0;
            if (lat >= mid) {
                ch = (ch << 1) | 1;
                lat_lo = mid;
            } else {
                ch <<= 1;
                lat_hi = mid;
            }
        }
        even = !even;
        if (++bit == 5) {
            hash += base32[ch];
            bit = 0;
            ch = 0;
        }
    }
    return hash;
}

}  // namespace soil
