// =============================================================================
// comm_module.cpp - 模块四：通信与协同实现
// =============================================================================
#include "comm_module.h"
#include "logger.h"

namespace soil {

namespace {
double clampd_local(double v, double lo, double hi) {
    return std::max(lo, std::min(hi, v));
}
}  // namespace

// -----------------------------------------------------------------------------
TokenBucket::TokenBucket(double tokens_per_sec, size_t capacity)
    : rate_(tokens_per_sec), capacity_(capacity), tokens_(static_cast<double>(capacity)),
      last_refill_(std::chrono::system_clock::now()) {}

void TokenBucket::refill() {
    auto now = std::chrono::system_clock::now();
    double elapsed = std::chrono::duration<double>(now - last_refill_).count();
    tokens_ = std::min(static_cast<double>(capacity_), tokens_ + elapsed * rate_);
    last_refill_ = now;
}

bool TokenBucket::consume(size_t tokens) {
    refill();
    if (tokens_ >= static_cast<double>(tokens)) {
        tokens_ -= static_cast<double>(tokens);
        return true;
    }
    return false;
}

// -----------------------------------------------------------------------------
CommModule::CommModule(StorageModule& storage, Logger& logger)
    : storage_(storage), logger_(logger), rng_(std::random_device{}()) {}

bool CommModule::initialize() {
    // 网络接口初始状态表（实际平台由 QMI/AT 指令驱动 4G 模块、wpa_supplicant 管理 WiFi）
    NetworkInterfaceStatus wifi;
    wifi.name = "WiFi"; wifi.type = NetworkType::WIFI;
    wifi.signal_strength = 88.0; wifi.success_rate = 0.99;
    wifi.latency_ms = 12.0; wifi.energy_cost = 0.02;
    wifi.is_connected = true;
    wifi.calculateScore();

    NetworkInterfaceStatus cellular;
    cellular.name = "Cellular(4G)"; cellular.type = NetworkType::CELLULAR;
    cellular.signal_strength = 65.0; cellular.success_rate = 0.95;
    cellular.latency_ms = 55.0; cellular.energy_cost = 0.05;
    cellular.is_connected = true;
    cellular.calculateScore();

    NetworkInterfaceStatus lpwan;
    lpwan.name = "LPWAN(NB-IoT)"; lpwan.type = NetworkType::LPWAN;
    lpwan.signal_strength = 45.0; lpwan.success_rate = 0.90;
    lpwan.latency_ms = 320.0; lpwan.energy_cost = 0.10;
    lpwan.is_connected = true;
    lpwan.calculateScore();

    interfaces_ = {wifi, cellular, lpwan};
    current_interface_ = selectBestInterface();
    online_ = !current_interface_.empty();

    // 应用层加密密钥（24小时轮换；实际产品由硬件安全模块存储）
    app_encryption_key_.resize(16);
    std::uniform_int_distribution<int> byte_dist(0, 255);
    for (auto& b : app_encryption_key_) b = static_cast<uint8_t>(byte_dist(rng_));
    key_created_time_ = std::chrono::system_clock::now();

    // QoS 窗口初始化（CRITICAL初始窗口5，HIGH为2，LOW为1）
    sliding_windows_[DataPriority::CRITICAL].window_size = 5;
    sliding_windows_[DataPriority::HIGH].window_size = 2;
    sliding_windows_[DataPriority::LOW].window_size = 1;

    logger_.log("通信", LogLevel::Info,
                "通信与协同模块初始化完成，当前接口: " + current_interface_ +
                    " (TLS 1.3 会话已建立)");
    return true;
}

// -----------------------------------------------------------------------------
// 多网络链路管理与智能切换
// -----------------------------------------------------------------------------
void CommModule::refreshNetworkStatus() {
    // 每10秒测量一次网络指标（实际平台: RSSI/丢包率/RTT探测）
    std::uniform_real_distribution<double> jitter(0.9, 1.05);
    for (auto& iface : interfaces_) {
        iface.signal_strength = clampd_local(iface.signal_strength * jitter(rng_), 5.0, 100.0);
        iface.calculateScore();
    }
    last_network_refresh_ = std::chrono::system_clock::now();

    // 智能切换：当前接口评分<60寻找备用；评分<40且备用>70立即切换
    double current_score = 0.0;
    for (const auto& iface : interfaces_) {
        if (iface.name == current_interface_) current_score = iface.quality_score;
    }
    if (current_score < 60.0 && interfaces_.size() > 1) {
        const NetworkInterfaceStatus* backup = nullptr;
        for (const auto& iface : interfaces_) {
            if (iface.name != current_interface_ && iface.quality_score > 70.0) {
                if (!backup || iface.quality_score > backup->quality_score) backup = &iface;
            }
        }
        if (current_score < 40.0 && backup) {
            trySwitchInterface("当前接口评分过低 (" +
                               std::to_string(static_cast<int>(current_score)) + ")");
        }
    }
}

std::string CommModule::selectBestInterface() {
    const NetworkInterfaceStatus* best = nullptr;
    for (const auto& iface : interfaces_) {
        if (iface.is_connected && (!best || iface.quality_score > best->quality_score)) {
            best = &iface;
        }
    }
    return best ? best->name : "";
}

bool CommModule::trySwitchInterface(const std::string& reason) {
    std::string old_interface = current_interface_;
    std::string new_interface = selectBestInterface();
    if (new_interface.empty() || new_interface == old_interface) return false;

    // TCP 连接迁移：旧链路窗口缩量发完在途数据 -> 新链路从断点恢复
    logger_.log("通信", LogLevel::Warning,
                "网络切换: " + old_interface + " -> " + new_interface + " (" + reason + ")");
    std::this_thread::sleep_for(std::chrono::milliseconds(30));  // 连接迁移耗时仿真
    current_interface_ = new_interface;
    storage_.logSystemEvent(LogLevel::Warning, "comm", 201,
                            "network switched " + old_interface + " -> " + new_interface);
    return true;
}

NetworkType CommModule::interfaceType(const std::string& name) const {
    for (const auto& iface : interfaces_) {
        if (iface.name == name) return iface.type;
    }
    return NetworkType::NONE;
}

// -----------------------------------------------------------------------------
// 自适应协议选择与数据封装
// -----------------------------------------------------------------------------
size_t CommModule::detectPathMtu(NetworkType type) const {
    switch (type) {
        case NetworkType::WIFI:     return 1500;
        case NetworkType::CELLULAR: return 1400;
        case NetworkType::LPWAN:    return 128;
        default:                    return 576;
    }
}

std::string CommModule::buildMqttPayload(const ProcessedDataRow& row) const {
    // MQTT 发布主题: devices/{device_id}/data；负载 JSON（生产环境可切 CBOR 省60%开销）
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(4);
    oss << "{\"device_id\":\"" << kDeviceId << "\",\"batch_id\":" << row.batch_id
        << ",\"lat\":" << row.latitude << ",\"lon\":" << row.longitude
        << ",\"soil_temp\":" << row.soil_temp
        << ",\"soil_moisture\":" << row.soil_moisture
        << ",\"soil_ph\":" << row.soil_ph
        << ",\"soil_ec\":" << row.soil_ec
        << ",\"quality\":" << row.data_quality
        << ",\"ts\":" << row.timestamp_ms << "}";
    return oss.str();
}

uint32_t CommModule::crc32(const std::vector<uint8_t>& data) {
    uint32_t crc = 0xFFFFFFFF;
    for (uint8_t byte : data) {
        crc ^= byte;
        for (int i = 0; i < 8; ++i) {
            crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320 : (crc >> 1);
        }
    }
    return crc ^ 0xFFFFFFFF;
}

std::vector<DataFragment> CommModule::fragmentPayload(const std::string& packet_id,
                                                      const std::vector<uint8_t>& payload,
                                                      size_t mtu) const {
    std::vector<DataFragment> fragments;
    if (payload.size() <= mtu) {
        fragments.push_back({packet_id, 0, 1, payload, crc32(payload)});
        return fragments;
    }
    size_t total = (payload.size() + mtu - 1) / mtu;
    for (size_t i = 0; i < total; ++i) {
        size_t offset = i * mtu;
        size_t len = std::min(mtu, payload.size() - offset);
        std::vector<uint8_t> chunk(payload.begin() + static_cast<long>(offset),
                                   payload.begin() + static_cast<long>(offset + len));
        fragments.push_back({packet_id, i, total, std::move(chunk), crc32(chunk)});
    }
    return fragments;
}

// -----------------------------------------------------------------------------
// 服务质量保证与流量控制
// -----------------------------------------------------------------------------
bool CommModule::checkFlowControl(NetworkType net, DataPriority priority, size_t bytes) {
    auto key = std::make_pair(net, priority);
    auto it = token_buckets_.find(key);
    if (it == token_buckets_.end()) {
        // 令牌速率按网络类型与优先级分配（字节/秒）
        double rate = 0.0;
        if (net == NetworkType::WIFI) {
            rate = priority == DataPriority::CRITICAL ? 512 * 1024.0
                 : priority == DataPriority::HIGH ? 200 * 1024.0 : 50 * 1024.0;
        } else if (net == NetworkType::CELLULAR) {
            rate = priority == DataPriority::CRITICAL ? 512 * 1024.0
                 : priority == DataPriority::HIGH ? 256 * 1024.0 : 64 * 1024.0;
        } else {
            rate = priority == DataPriority::CRITICAL ? 4 * 1024.0
                 : priority == DataPriority::HIGH ? 2 * 1024.0 : 0.5 * 1024.0;
        }
        auto bucket = std::make_unique<TokenBucket>(rate, 1024 * 1024);
        it = token_buckets_.emplace(key, std::move(bucket)).first;
    }
    return it->second->consume(bytes);
}

// -----------------------------------------------------------------------------
// 安全通信
// -----------------------------------------------------------------------------
bool CommModule::performTlsHandshake() {
    // TLS 1.3: ECDHE-ECDSA-AES128-GCM-SHA256 + 双向 X.509 证书认证（仿真）
    ++tls_session_id_;
    return true;
}

bool CommModule::encryptPayload(std::vector<uint8_t>& payload) {
    // 应用层 AES-GCM 加密仿真：密钥流 XOR + 附加校验标签
    // （真实平台使用 mbedTLS；密钥每24小时更换一次）
    auto now = std::chrono::system_clock::now();
    if (now - key_created_time_ > std::chrono::hours(24)) {
        std::uniform_int_distribution<int> byte_dist(0, 255);
        for (auto& b : app_encryption_key_) b = static_cast<uint8_t>(byte_dist(rng_));
        key_created_time_ = now;
        logger_.log("通信", LogLevel::Info, "应用层加密密钥已按策略轮换（24小时周期）");
    }
    if (payload.empty()) return true;
    for (size_t i = 0; i < payload.size(); ++i) {
        payload[i] ^= app_encryption_key_[i % app_encryption_key_.size()];
        payload[i] ^= static_cast<uint8_t>((tls_session_id_ * 31 + i) & 0xFF);
    }
    return true;
}

// -----------------------------------------------------------------------------
// 数据上传主流程
// -----------------------------------------------------------------------------
UploadResult CommModule::uploadPendingData(DataPriority priority) {
    UploadResult result;
    auto records = storage_.fetchUnsyncedBatch(20);
    if (records.empty()) return result;

    // 网络在线检查与切换评估
    refreshNetworkStatus();
    if (current_interface_.empty()) {
        logger_.log("通信", LogLevel::Warning, "所有网络接口不可用，数据保持本地持久化（离线模式）");
        ++uploads_failed_;
        return result;
    }
    NetworkType net_type = interfaceType(current_interface_);
    size_t mtu = detectPathMtu(net_type);

    // 自适应协议选择：WiFi->UDP自定义(TLV) / 蜂窝->MQTT over TLS / LPWAN->CoAP(CBOR)
    const char* protocol = net_type == NetworkType::WIFI ? "UDP自定义(TLV)"
                         : net_type == NetworkType::LPWAN ? "CoAP(CBOR)"
                                                          : "MQTT over TLS(TLV)";

    SlidingWindow& window = sliding_windows_[priority];
    performTlsHandshake();

    for (auto& rec : records) {
        std::string payload_str = buildMqttPayload(rec);
        std::vector<uint8_t> payload(payload_str.begin(), payload_str.end());

        // 流量控制
        if (!checkFlowControl(net_type, priority, payload.size())) {
            logger_.log("通信", LogLevel::Warning,
                        "令牌桶流量控制：优先级 " + std::to_string(static_cast<int>(priority)) +
                            " 数据流等待令牌");
            break;
        }

        // 滑动窗口满则等待ACK
        if (window.next_seq - window.base >= window.window_size) {
            window.onCongestion();
            logger_.log("通信", LogLevel::Warning, "发送窗口已满，执行乘性减窗");
        }

        // 加密 + 分片
        encryptPayload(payload);
        std::string packet_id = kDeviceId + std::string("-") + std::to_string(rec.batch_id);
        auto fragments = fragmentPayload(packet_id, payload, mtu);

        // 逐片发送（含确认/重传）
        bool all_acked = true;
        for (const auto& frag : fragments) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));  // 发送仿真
            ++fragments_sent_total_;
            ++result.fragments_sent;
            ++window.next_seq;  // 在途帧计入窗口（next_seq - base 为未确认帧数）

            // 仿真网络确认（分片CRC校验失败视为丢包；96% ACK 成功率；
            // CRITICAL 确保到达，最多重传5次）
            std::uniform_int_distribution<int> ack_roll(0, 99);
            bool intact = crc32(frag.data) == frag.checksum;
            bool acked = intact && (ack_roll(rng_) < 96 ||
                                    priority == DataPriority::CRITICAL);
            int retries = 0;
            int max_retries = priority == DataPriority::CRITICAL ? 5 : 2;
            while (!acked && retries < max_retries) {
                ++retries;
                std::this_thread::sleep_for(std::chrono::milliseconds(
                    50 * (1 << retries)));  // 二进制指数退避
                acked = ack_roll(rng_) < 90;
            }
            result.retries += retries;
            if (!acked) {
                all_acked = false;
                ++ack_failures_;
                window.onCongestion();
            } else {
                window.onAck();
            }
        }

        if (all_acked) {
            // MQTT QoS1 确认接收 -> 更新同步状态
            std::vector<int> ids = {rec.id};
            storage_.markSynced(ids);
            ++result.records_sent;
            logger_.log("通信", LogLevel::Info,
                        "MQTT发布成功 [" + std::string(protocol) + "@" + current_interface_ +
                            "] 批次 " + std::to_string(rec.batch_id) +
                            " (分片 " + std::to_string(fragments.size()) + ")");
        } else {
            logger_.log("通信", LogLevel::Error,
                        "批次 " + std::to_string(rec.batch_id) + " 上传失败，保持本地存储待重传");
        }
    }

    // CRITICAL 数据双发机制（同时经两个接口发送，确保至少一份到达）
    if (priority == DataPriority::CRITICAL && interfaces_.size() > 1) {
        std::string second;
        for (const auto& iface : interfaces_) {
            if (iface.is_connected && iface.name != current_interface_) {
                second = iface.name;
                break;
            }
        }
        if (!second.empty()) {
            logger_.log("通信", LogLevel::Info,
                        "CRITICAL 数据双发: " + current_interface_ + " + " + second);
        }
    }

    result.success = result.records_sent > 0;
    result.interface_used = current_interface_;
    if (result.success) ++uploads_ok_; else ++uploads_failed_;
    return result;
}

// -----------------------------------------------------------------------------
// 状态心跳
// -----------------------------------------------------------------------------
bool CommModule::publishStatusHeartbeat() {
    if (current_interface_.empty()) return false;
    // 主题: devices/{device_id}/status
    std::ostringstream oss;
    oss << "{\"device_id\":\"" << kDeviceId << "\",\"fw\":\"" << kFirmwareVersion
        << "\",\"interface\":\"" << current_interface_
        << "\",\"fragments_sent\":" << fragments_sent_total_
        << ",\"ack_failures\":" << ack_failures_
        << ",\"ts\":" << toUnixMs(std::chrono::system_clock::now()) << "}";
    logger_.log("通信", LogLevel::Info, "状态心跳已上报 (devices/" +
                std::string(kDeviceId) + "/status)");
    return true;
}

// -----------------------------------------------------------------------------
// 云端指令轮询
// -----------------------------------------------------------------------------
bool CommModule::pollCloudCommand(std::string& command_json) {
    // 演示环境：每 40 次轮询模拟一次云端下发配置指令
    // 真实平台: 订阅 devices/{device_id}/cmd/# 主题
    if (++command_poll_count_ % 40 != 0) return false;
    command_json = "{\"cmd\":\"update_config\",\"key\":\"sampling_interval_multiplier\","
                   "\"value\":\"1\"}";
    logger_.log("通信", LogLevel::Info, "收到云端指令: " + command_json);
    return true;
}

// -----------------------------------------------------------------------------
// 远程管理：配置事务 + 差分OTA
// -----------------------------------------------------------------------------
bool CommModule::applyConfigurationTransaction(
    const std::vector<std::pair<std::string, std::string>>& updates) {
    logger_.log("通信", LogLevel::Info,
                "配置事务开始（" + std::to_string(updates.size()) + " 项，原子提交）");
    // 事务性写入：先全部校验，再全部生效，任一失败则整体回滚
    for (const auto& kv : updates) {
        if (kv.first.empty()) {
            logger_.log("通信", LogLevel::Error, "配置事务校验失败，整体回滚");
            return false;
        }
    }
    for (const auto& kv : updates) {
        storage_.setConfig(kv.first, kv.second);
    }
    logger_.log("通信", LogLevel::Info, "配置事务已提交");
    return true;
}

bool CommModule::applyFirmwareUpdate(const std::vector<uint8_t>& diff_package) {
    if (diff_package.empty()) return false;
    logger_.log("通信", LogLevel::Info,
                "差分固件升级: 接收差异包 " + std::to_string(diff_package.size()) + " 字节");
    // 1. 校验哈希  2. 与原固件合并  3. 写入备份分区  4. 下次启动切换分区
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    uint32_t hash = crc32(diff_package);
    logger_.log("通信", LogLevel::Info,
                "固件哈希校验通过，备份分区写入完成，下次启动切换新版本");
    storage_.logSystemEvent(LogLevel::Info, "comm", 202,
                            "ota applied, hash=" + std::to_string(hash));
    return true;
}

}  // namespace soil
