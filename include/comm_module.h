// =============================================================================
// comm_module.h - 模块四：通信与协同
//
// 职责：
//   1. 多网络链路管理与智能切换（综合评分 / 双发机制 / TCP连接迁移）
//   2. 自适应协议选择与数据封装（UDP-TLV / MQTT-TLS / CoAP-CBOR / 智能分片）
//   3. 服务质量保证与流量控制（三级QoS / 滑动窗口拥塞控制 / 令牌桶）
//   4. 安全通信与远程管理（TLS1.3 / 双向证书 / 配置事务 / 差分OTA）
// =============================================================================
#pragma once

#include "common.h"
#include "data_queue.h"
#include "storage_module.h"

namespace soil {

class Logger;

// 网络接口类型
enum class NetworkType { WIFI, CELLULAR, LPWAN, NONE };
inline const char* networkTypeName(NetworkType t) {
    switch (t) {
        case NetworkType::WIFI: return "WiFi";
        case NetworkType::CELLULAR: return "Cellular(4G)";
        case NetworkType::LPWAN: return "LPWAN(NB-IoT)";
        default: return "离线";
    }
}

// 数据优先级
enum class DataPriority { CRITICAL = 0, HIGH = 1, LOW = 2 };

// 网络接口状态（评分：信号30% + 成功率40% + 延迟20% + 能耗10%）
struct NetworkInterfaceStatus {
    std::string name;
    NetworkType type = NetworkType::NONE;
    bool is_connected = false;
    double signal_strength = 0.0;   // 0~100
    double success_rate = 0.0;      // 0~1
    double latency_ms = 0.0;
    double energy_cost = 0.0;       // 成本/MB
    double quality_score = 0.0;

    void calculateScore() {
        double n_sig = signal_strength / 100.0;
        double n_succ = success_rate;
        double n_lat = 1.0 - std::min(latency_ms / 500.0, 1.0);
        double n_cost = 1.0 - std::min(energy_cost / 0.1, 1.0);
        quality_score = 30.0 * n_sig + 40.0 * n_succ + 20.0 * n_lat + 10.0 * n_cost;
    }
};

// 滑动窗口（加性增/乘性减拥塞控制）
struct SlidingWindow {
    size_t window_size = 2;
    size_t base = 0;
    size_t next_seq = 0;
    int consecutive_acks = 0;

    void onAck() {
        ++consecutive_acks;
        if (consecutive_acks >= 10) {
            ++window_size;
            consecutive_acks = 0;
        }
        ++base;
    }
    void onCongestion() {
        window_size = std::max<size_t>(1, window_size / 2);
        consecutive_acks = 0;
    }
};

// 令牌桶流量控制
class TokenBucket {
public:
    TokenBucket(double tokens_per_sec, size_t capacity);
    bool consume(size_t tokens);

private:
    void refill();
    double rate_;
    size_t capacity_;
    double tokens_;
    std::chrono::system_clock::time_point last_refill_;
};

// 数据分片
struct DataFragment {
    std::string packet_id;
    size_t fragment_index = 0;
    size_t total_fragments = 0;
    std::vector<uint8_t> data;
    uint32_t checksum = 0;
};

// 上传结果
struct UploadResult {
    bool success = false;
    std::string interface_used;
    int records_sent = 0;
    int fragments_sent = 0;
    int retries = 0;
    bool switched_network = false;
};

class CommModule {
public:
    CommModule(StorageModule& storage, Logger& logger);

    bool initialize();

    // 数据存储与转发线程调用：从数据库取待同步记录并上传（MQTT 发布仿真）
    UploadResult uploadPendingData(DataPriority priority);

    // 状态心跳上报（定时任务线程调用）
    bool publishStatusHeartbeat();

    // 云端指令接收（网络与指令监听线程轮询）
    // 返回 true 表示收到指令（演示环境周期性模拟云端下发配置）
    bool pollCloudCommand(std::string& command_json);

    // 网络质量周期测量（10秒周期）
    void refreshNetworkStatus();

    bool isOnline() const { return online_; }
    std::string currentInterface() const { return current_interface_; }

    // 差分固件升级（远程管理）
    bool applyFirmwareUpdate(const std::vector<uint8_t>& diff_package);
    // 配置事务（原子提交/回滚）
    bool applyConfigurationTransaction(
        const std::vector<std::pair<std::string, std::string>>& updates);

    int fragments_sent_total() const { return fragments_sent_total_; }
    int ack_failures() const { return ack_failures_; }

private:
    NetworkType interfaceType(const std::string& name) const;
    std::string selectBestInterface();
    bool trySwitchInterface(const std::string& reason);

    size_t detectPathMtu(NetworkType type) const;
    std::string buildMqttPayload(const ProcessedDataRow& row) const;
    std::vector<DataFragment> fragmentPayload(const std::string& packet_id,
                                              const std::vector<uint8_t>& payload,
                                              size_t mtu) const;
    static uint32_t crc32(const std::vector<uint8_t>& data);

    bool checkFlowControl(NetworkType net, DataPriority priority, size_t bytes);

    bool performTlsHandshake();
    bool encryptPayload(std::vector<uint8_t>& payload);

    StorageModule& storage_;
    Logger& logger_;

    std::vector<NetworkInterfaceStatus> interfaces_;
    std::string current_interface_;
    bool online_ = false;
    std::chrono::system_clock::time_point last_network_refresh_;

    // QoS 状态
    std::map<DataPriority, SlidingWindow> sliding_windows_;
    std::map<std::pair<NetworkType, DataPriority>, std::unique_ptr<TokenBucket>> token_buckets_;

    // TLS 会话与应用层加密密钥（24小时轮换）
    int tls_session_id_ = 0;
    std::vector<uint8_t> app_encryption_key_;
    std::chrono::system_clock::time_point key_created_time_;

    // 统计
    int fragments_sent_total_ = 0;
    int ack_failures_ = 0;
    int uploads_ok_ = 0;
    int uploads_failed_ = 0;
    std::mt19937 rng_;

    // 云端指令模拟
    int command_poll_count_ = 0;
};

}  // namespace soil
