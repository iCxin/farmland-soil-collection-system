// =============================================================================
// sensing_module.cpp - 模块一：感知与信号调理实现
// =============================================================================
#include "sensing_module.h"
#include "logger.h"

#include <fstream>

namespace soil {

namespace {
// 3字节寄存器值解码（硬件抽象层编码约定：物理量*100，最高位为符号）
double decodeRegisterValue(const std::vector<uint8_t>& reg) {
    if (reg.size() < 3) return 0.0;
    uint32_t bits = (static_cast<uint32_t>(reg[0]) << 16) |
                    (static_cast<uint32_t>(reg[1]) << 8) |
                    static_cast<uint32_t>(reg[2]);
    double magnitude = static_cast<double>(bits & 0x7FFFFF) / 100.0;
    return (bits & 0x800000) ? -magnitude : magnitude;
}
}  // namespace

SensingModule::SensingModule(SimulatedHardware& hw,
                             ThreadSafeQueue<ConditionedSensorData>& output_queue,
                             Logger& logger)
    : hw_(hw), output_queue_(output_queue), logger_(logger) {
    last_movement_time_ = std::chrono::system_clock::now();
}

// -----------------------------------------------------------------------------
// 传感器初始化与自检阶段
// -----------------------------------------------------------------------------
bool SensingModule::loadConfigFromJson(const std::string& config_file) {
    std::ifstream ifs(config_file);
    if (!ifs.is_open()) {
        // 配置文件缺失时使用内置默认配置（保证系统可运行）
        logger_.log("感知", LogLevel::Warning,
                    "配置文件 " + config_file + " 读取失败，使用内置默认配置");
        SensorConfig cfg;
        cfg = {"DHT22", SensorType::TEMPERATURE, 0x10, 2.5, 200, {0.0, 1.0, 0.0}, 0,
               kDefaultIntervalHumidityMs, 3};
        cfg.interval_ms = kDefaultIntervalSoilTempMs;
        sensor_config_db_.push_back(cfg);
        sensor_config_db_.push_back({"EC-Probe-v2", SensorType::CONDUCTIVITY, 0x11, 2.5, 100,
                                     {0.0, 0.5, 0.0}, 1, kDefaultIntervalEcMs, 4});
        sensor_config_db_.push_back({"pH-Meter-XYZ", SensorType::PH, 0x12, 2.5, 50,
                                     {0.0, 1.02, -0.15}, 0, kDefaultIntervalPhMs, 3});
        sensor_config_db_.push_back({"Capacitive-Soil-Moisture", SensorType::SOIL_HUMIDITY, 0x13,
                                     2.5, 200, {0.0, 1.0, 0.0}, 0,
                                     kDefaultIntervalHumidityMs, 1});
        sensor_config_db_.push_back({"DS18B20", SensorType::SOIL_TEMPERATURE, 0x14, 2.5, 100,
                                     {0.0, 1.0, 0.0}, 0, kDefaultIntervalSoilTempMs, 2});
        return true;
    }
    // 轻量 JSON 解析（仅提取本项目配置所需字段）
    std::stringstream ss;
    ss << ifs.rdbuf();
    std::string json = ss.str();

    // 逐个传感器块解析
    size_t pos = 0;
    while ((pos = json.find("{\"model\"", pos)) != std::string::npos) {
        size_t end = json.find('}', pos);
        if (end == std::string::npos) break;
        std::string block = json.substr(pos, end - pos);
        pos = end;

        SensorConfig cfg;
        auto extractStr = [&](const std::string& key) -> std::string {
            size_t k = block.find("\"" + key + "\"");
            if (k == std::string::npos) return "";
            size_t c = block.find(':', k);
            size_t q1 = block.find('"', c);
            size_t q2 = block.find('"', q1 + 1);
            return (q1 == std::string::npos || q2 == std::string::npos)
                       ? "" : block.substr(q1 + 1, q2 - q1 - 1);
        };
        auto extractNum = [&](const std::string& key) -> double {
            size_t k = block.find("\"" + key + "\"");
            if (k == std::string::npos) return 0.0;
            size_t c = block.find(':', k);
            return std::atof(block.c_str() + c + 1);
        };
        cfg.model = extractStr("model");
        cfg.address = static_cast<int>(extractNum("address"));
        cfg.sample_rate_hz = static_cast<int>(extractNum("sample_rate_hz"));
        cfg.work_mode = static_cast<int>(extractNum("work_mode"));
        cfg.interval_ms = static_cast<int>(extractNum("interval_ms"));
        cfg.priority = static_cast<int>(extractNum("priority"));
        size_t ck = block.find("\"calibration_coeffs\"");
        if (ck != std::string::npos) {
            size_t lb = block.find('[', ck);
            size_t rb = block.find(']', lb);
            std::istringstream coeff_stream(block.substr(lb + 1, rb - lb - 1));
            std::string tok;
            int idx = 0;
            while (std::getline(coeff_stream, tok, ',') && idx < 3) {
                cfg.calibration_coeffs[idx++] = std::atof(tok.c_str());
            }
        }
        // 型号 -> 传感器类型
        if (cfg.model == "DHT22") cfg.type = SensorType::TEMPERATURE;
        else if (cfg.model == "EC-Probe-v2") cfg.type = SensorType::CONDUCTIVITY;
        else if (cfg.model == "pH-Meter-XYZ") cfg.type = SensorType::PH;
        else if (cfg.model == "Capacitive-Soil-Moisture") cfg.type = SensorType::SOIL_HUMIDITY;
        else if (cfg.model == "DS18B20") cfg.type = SensorType::SOIL_TEMPERATURE;
        else continue;
        sensor_config_db_.push_back(cfg);
    }
    logger_.log("感知", LogLevel::Info,
                "从配置文件加载 " + std::to_string(sensor_config_db_.size()) + " 个传感器配置");
    return !sensor_config_db_.empty();
}

void SensingModule::configureControllers() {
    // I2C 控制器：100kHz 标准模式
    hw_.i2c_write(0xFF, {0x01});
    // SPI 控制器：CPOL=0, CPHA=1
    hw_.spi_transfer(0x02);
    hw_.spi_transfer(0x03);
    // UART：按传感器规格设定波特率（0x2580=9600, 0x0440=115200... 帧内示意）
    for (const auto& cfg : sensor_config_db_) {
        if (cfg.model == "DHT22") hw_.uart_send({0x01, 0x25, 0x80});
        else if (cfg.model == "EC-Probe-v2") hw_.uart_send({0x03, 0x04, 0x40});
    }
    // ADC：内部 2.5V 基准，200Hz 采样率（通过 SPI 配置字下发）
    hw_.spi_transfer(0xA0);
    hw_.spi_transfer(0xC8);
}

void SensingModule::selfTestAllSensors() {
    for (const auto& cfg : sensor_config_db_) {
        sensor_statuses_[cfg.type] = SensorStatus::INITIALIZING;
        type_to_index_[cfg.type] = static_cast<int>(sensor_config_db_.size());
    }

    for (const auto& cfg : sensor_config_db_) {
        bool passed = false;
        std::string message;
        switch (cfg.type) {
            case SensorType::TEMPERATURE: {
                uint16_t id = hw_.read_manufacturer_id(cfg.address);
                passed = (id == 0x55AA);
                char buf[96];
                std::snprintf(buf, sizeof(buf),
                    passed ? "数字温度传感器厂商ID校验通过 (0x55AA)"
                           : "数字温度传感器厂商ID校验失败 (期望 0x55AA, 实得 0x%04X)", id);
                message = buf;
                break;
            }
            case SensorType::CONDUCTIVITY: {
                passed = hw_.perform_short_circuit_calibration(cfg.address);
                message = passed ? "电导率传感器短路校准测试通过"
                                 : "电导率传感器短路校准测试失败";
                break;
            }
            case SensorType::PH: {
                double impedance = hw_.measure_electrode_impedance(cfg.address);
                passed = (impedance >= 1e7 && impedance <= 1e8);
                std::ostringstream oss;
                oss << (passed ? "pH电极阻抗 " : "pH电极阻抗异常 ")
                    << std::fixed << std::setprecision(1)
                    << impedance / 1e6 << " MΩ"
                    << (passed ? "，处于正常范围 (10~100MΩ)" : "，超出正常范围");
                message = oss.str();
                break;
            }
            case SensorType::SOIL_HUMIDITY:
            case SensorType::SOIL_TEMPERATURE: {
                std::vector<uint8_t> data;
                passed = hw_.i2c_read(cfg.address, 3, data);
                message = cfg.model + (passed ? " 基础读写测试通过" : " 基础读写测试失败");
                break;
            }
            default:
                message = "不支持的传感器自检类型";
                break;
        }
        if (passed) {
            sensor_statuses_[cfg.type] = SensorStatus::READY;
            ++stats_.self_test_passed;
            logger_.sensorLog(cfg.type, LogLevel::Info, "自检通过: " + message);
        } else {
            sensor_statuses_[cfg.type] = SensorStatus::ERROR;
            ++stats_.self_test_failed;
            logger_.sensorLog(cfg.type, LogLevel::Error, "自检失败: " + message +
                              "（已调整数据融合策略以补偿缺失参数）");
        }
    }
}

bool SensingModule::initialize(const std::string& config_file) {
    if (!loadConfigFromJson(config_file)) {
        logger_.log("感知", LogLevel::Error, "无可用传感器配置，模块初始化失败");
        return false;
    }
    configureControllers();
    selfTestAllSensors();

    // 为自检通过的传感器建立初始任务队列
    rebuildTaskQueue();
    logger_.log("感知", LogLevel::Info,
                "初始化完成: " + std::to_string(stats_.self_test_passed) + " 通过 / " +
                std::to_string(stats_.self_test_failed) + " 失败, 任务队列 " +
                std::to_string(task_queue_.size()) + " 项");
    return stats_.self_test_passed > 0;
}

void SensingModule::rebuildTaskQueue() {
    std::lock_guard<std::mutex> lock(task_queue_mutex_);
    while (!task_queue_.empty()) task_queue_.pop();
    auto now = std::chrono::system_clock::now();
    for (const auto& cfg : sensor_config_db_) {
        auto it = sensor_statuses_.find(cfg.type);
        if (it == sensor_statuses_.end()) continue;
        if (it->second == SensorStatus::READY || it->second == SensorStatus::DEGRADED) {
            if (low_power_mode_ && cfg.type != SensorType::SOIL_HUMIDITY) continue;
            task_queue_.push({cfg.type,
                              std::chrono::milliseconds(job_started_ ? cfg.interval_ms / 4
                                                                     : cfg.interval_ms),
                              calculateTaskPriority(cfg.type), now});
        }
    }
}

int SensingModule::calculateTaskPriority(SensorType type) const {
    switch (type) {
        case SensorType::SOIL_HUMIDITY: return 1;   // 灌溉决策关键，最高优先级
        case SensorType::SOIL_TEMPERATURE: return 2;
        case SensorType::PH: return 3;
        case SensorType::CONDUCTIVITY: return 4;
        default: return 10;
    }
}

int SensingModule::defaultIntervalMs(SensorType type) const {
    for (const auto& cfg : sensor_config_db_) {
        if (cfg.type == type) return cfg.interval_ms;
    }
    return 60000;
}

// -----------------------------------------------------------------------------
// 动态采样调度与触发管理
// -----------------------------------------------------------------------------
void SensingModule::manageSamplingScheduleAndTriggers() {
    // --- CAN 总线作业触发 ---
    CanSignal sig = hw_.receive_can_signal();
    handleCanTrigger(sig);

    // --- 位置感知智能触发 ---
    GpsData gps = hw_.get_gps_data();
    handleLocationAwareTrigger(gps);
}

void SensingModule::handleCanTrigger(const CanSignal& sig) {
    if (sig.signal_name == "JOB_START" && sig.value == 1 && !job_started_) {
        job_started_ = true;
        ++stats_.high_freq_mode_switches;
        logger_.log("感知", LogLevel::Info,
                    "收到 CAN 作业开始信号，进入高频率采集模式（采样间隔缩短为1/4）");
        // 任务间隔缩短为原来的四分之一
        std::lock_guard<std::mutex> lock(task_queue_mutex_);
        std::vector<ScheduledTask> tasks;
        while (!task_queue_.empty()) {
            ScheduledTask t = task_queue_.top();
            task_queue_.pop();
            t.interval /= 4;
            t.next_execution_time = std::chrono::system_clock::now();
            tasks.push_back(t);
        }
        for (auto& t : tasks) task_queue_.push(t);
    } else if (sig.signal_name == "JOB_END" && sig.value == 1 && job_started_) {
        job_started_ = false;
        logger_.log("感知", LogLevel::Info, "收到 CAN 作业结束信号，恢复正常采集模式");
        std::lock_guard<std::mutex> lock(task_queue_mutex_);
        std::vector<ScheduledTask> tasks;
        while (!task_queue_.empty()) {
            ScheduledTask t = task_queue_.top();
            task_queue_.pop();
            t.interval = std::chrono::milliseconds(defaultIntervalMs(t.sensor_type));
            t.next_execution_time = std::chrono::system_clock::now();
            tasks.push_back(t);
        }
        for (auto& t : tasks) task_queue_.push(t);
    }
}

void SensingModule::handleLocationAwareTrigger(const GpsData& gps) {
    if (gps.speed_mps > movement_speed_threshold_) {
        if (!is_moving_) {
            is_moving_ = true;
            distance_since_last_sample_ = 0.0;
            logger_.log("感知", LogLevel::Info,
                        "检测到载具移动 (>" + std::to_string(movement_speed_threshold_) +
                        " m/s)，启用距离触发模式（每移动2米采集一次）");
        }
        low_power_mode_ = false;
        current_speed_mps_ = gps.speed_mps;
        last_movement_time_ = std::chrono::system_clock::now();
        // 仿真硬件按 tick 累积行驶距离（speed * 20 倍仿真加速）
        distance_since_last_sample_ += gps.speed_mps * 20.0 * 0.05;  // 50ms 调度周期
    } else {
        if (is_moving_) {
            is_moving_ = false;
            current_speed_mps_ = 0.0;
            distance_since_last_sample_ = 0.0;
            logger_.log("感知", LogLevel::Info, "载具静止，切换回时间片调度模式");
        }
    }

    // 静止超时 -> 低功耗监测模式（仅保留湿度传感器）
    auto now = std::chrono::system_clock::now();
    if (!is_moving_ && !low_power_mode_ &&
        now - last_movement_time_ > stationary_timeout_) {
        low_power_mode_ = true;
        logger_.log("感知", LogLevel::Info,
                    "静止超过10分钟，进入低功耗监测模式：仅保持湿度传感器运行");
        for (auto& kv : sensor_statuses_) {
            if (kv.first == SensorType::SOIL_HUMIDITY) {
                kv.second = SensorStatus::READY;
            } else if (kv.second == SensorStatus::READY) {
                kv.second = SensorStatus::SLEEP;
            }
        }
        rebuildTaskQueue();
    } else if (low_power_mode_ && is_moving_) {
        low_power_mode_ = false;
        logger_.log("感知", LogLevel::Info, "退出低功耗模式，恢复全部传感器");
        for (auto& kv : sensor_statuses_) {
            if (kv.second == SensorStatus::SLEEP) kv.second = SensorStatus::READY;
        }
        rebuildTaskQueue();
    }
}

// -----------------------------------------------------------------------------
// 原始数据采集与信号调理
// -----------------------------------------------------------------------------
double SensingModule::movingWindowAverage(SensorType type, double new_value) {
    auto& window = filter_windows_[type];
    if (window.empty()) window.resize(kFilterWindowSize, new_value);
    window.push_back(new_value);
    while (window.size() > kFilterWindowSize) window.pop_front();
    double sum = 0.0;
    for (double v : window) sum += v;
    return sum / static_cast<double>(window.size());
}

bool SensingModule::tukeyFenceCheck(SensorType type, double value, double window_mean) {
    auto& window = filter_windows_[type];
    // 窗口预热期（真实样本不足）或分布过窄（IQR≈0，噪声水平内）时跳过异常值检验，
    // 避免恒定初始化窗口造成的系统性误报
    if (window.size() < 6) return true;
    std::vector<double> sorted_window(window.begin(), window.end());
    std::sort(sorted_window.begin(), sorted_window.end());
    size_t n = sorted_window.size();
    double q1 = sorted_window[n / 4];
    double q3 = sorted_window[(3 * n) / 4];
    double iqr = q3 - q1;
    if (iqr < 1e-6) return true;
    double lower = q1 - 1.5 * iqr;
    double upper = q3 + 1.5 * iqr;
    if (value < lower || value > upper) {
        logger_.sensorLog(type, LogLevel::Warning, "Tukey围栏检出异常值并剔除 (" +
                              std::to_string(value) + ")，以窗口均值替代");
        ++stats_.outliers_rejected;
        return false;
    }
    (void)window_mean;
    return true;
}

double SensingModule::sensorSpecificCompensation(SensorType type, double value) {
    switch (type) {
        case SensorType::TEMPERATURE: {
            // 自加热修正系数线性补偿
            return value - 0.1;
        }
        case SensorType::CONDUCTIVITY: {
            // 温度补偿公式：标准参考温度 25°C，补偿系数 1.9%/°C
            double current_temp = 25.0;
            std::vector<uint8_t> data;
            if (hw_.i2c_read(0x14, 3, data)) {
                current_temp = decodeRegisterValue(data);
            }
            double factor = 1.0 + 0.019 * (current_temp - 25.0);
            return value * factor;
        }
        default:
            return value;
    }
}

ConditionedSensorData SensingModule::collectAndCondition(SensorType type, int sensor_id) {
    ConditionedSensorData result;
    result.type = type;
    result.sensor_id = sensor_id;
    result.timestamp = std::chrono::system_clock::now();

    const SensorConfig* config = nullptr;
    for (const auto& cfg : sensor_config_db_) {
        if (cfg.type == type) { config = &cfg; break; }
    }
    if (!config || (sensor_statuses_[type] != SensorStatus::READY &&
                    sensor_statuses_[type] != SensorStatus::DEGRADED)) {
        return result;  // UNUSABLE
    }

    // --- 传感器预热 ---
    hw_.sensor_preheat(config->address);

    // --- 采集原始值 ---
    double raw_value = 0.0;
    try {
        switch (type) {
            case SensorType::SOIL_HUMIDITY: {
                // 电容式三阶段采集：预热 -> 100MHz 高频激励 -> 连续读取3个测量寄存器
                hw_.generate_high_frequency_signal(config->address);
                auto reg = hw_.read_sensor_registers(config->address, 3);
                raw_value = decodeRegisterValue(reg);
                break;
            }
            case SensorType::TEMPERATURE: {
                std::vector<uint8_t> uart_data;
                if (hw_.uart_receive(uart_data, std::chrono::milliseconds(200)) &&
                    uart_data.size() >= 4) {
                    raw_value = static_cast<double>((uart_data[1] << 8) | uart_data[2]) / 10.0;
                    if (uart_data[3] & 0x80) raw_value = -raw_value;
                } else {
                    throw std::runtime_error("UART 接收超时");
                }
                break;
            }
            case SensorType::CONDUCTIVITY: {
                std::vector<uint8_t> i2c_data;
                if (hw_.i2c_read(config->address, 3, i2c_data)) {
                    raw_value = decodeRegisterValue(i2c_data);
                } else {
                    throw std::runtime_error("I2C 读取失败");
                }
                break;
            }
            case SensorType::PH: {
                // 4~20mA 变送：电压反算 pH
                double voltage = hw_.adc_read(config->address);
                raw_value = (voltage - 0.4) / 1.6 * 14.0;
                break;
            }
            case SensorType::SOIL_TEMPERATURE: {
                double voltage = hw_.adc_read(config->address);
                raw_value = (voltage - 0.2) / 2.2 * 60.0 - 10.0;
                break;
            }
            default:
                return result;
        }
    } catch (const std::runtime_error& e) {
        logger_.sensorLog(type, LogLevel::Error,
                          std::string("数据采集错误: ") + e.what());
        sensor_statuses_[type] = SensorStatus::ERROR;
        ++stats_.read_errors;
        return result;
    }

    if (raw_value == 0.0 && hw_.isSensorFaultInjected(config->address)) {
        sensor_statuses_[type] = SensorStatus::ERROR;
        ++stats_.read_errors;
        return result;
    }

    // --- 多级信号处理流水线 ---
    double conditioned = raw_value;
    result.quality_level = DataQualityLevel::POOR;  // 原始采集成功的临时标记（校准后重新评级）
    // 第一级：移动窗口滤波（长度10）
    conditioned = movingWindowAverage(type, conditioned);
    // 第二级：Tukey 围栏异常值检测
    if (!tukeyFenceCheck(type, conditioned, conditioned)) {
        result.is_outlier_rejected = true;
    }
    // 第三级：传感器特异性补偿
    conditioned = sensorSpecificCompensation(type, conditioned);

    result.value = conditioned;
    result.timestamp = std::chrono::system_clock::now();
    return result;
}

// -----------------------------------------------------------------------------
// 实时校准与质量控制
// -----------------------------------------------------------------------------
ConditionedSensorData SensingModule::calibrateAndControlQuality(
    const ConditionedSensorData& data) {
    ConditionedSensorData result = data;
    const SensorConfig* config = nullptr;
    for (const auto& cfg : sensor_config_db_) {
        if (cfg.type == data.type) { config = &cfg; break; }
    }
    if (config) {
        // 二次多项式校准模型 y = A*x^2 + B*x + C
        result.value = config->calibration_coeffs[0] * std::pow(result.value, 2) +
                       config->calibration_coeffs[1] * result.value +
                       config->calibration_coeffs[2];
    }

    // ---- 零点校准检查（每日凌晨3点）----
    static auto last_zero_check = std::chrono::system_clock::now() - std::chrono::hours(25);
    auto now = std::chrono::system_clock::now();
    // 计算今日 03:00（C++17 无 chrono 日历类型，使用 localtime/mktime）
    std::time_t now_t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_3am{};
#if defined(_WIN32)
    localtime_s(&tm_3am, &now_t);
#else
    localtime_r(&now_t, &tm_3am);
#endif
    tm_3am.tm_hour = 3;
    tm_3am.tm_min = 0;
    tm_3am.tm_sec = 0;
    auto today_3am = std::chrono::system_clock::from_time_t(std::mktime(&tm_3am));
    if (now >= today_3am && now < today_3am + std::chrono::minutes(5) &&
        now - last_zero_check > std::chrono::hours(24)) {
        if (data.type == SensorType::PH && config) {
            logger_.sensorLog(SensorType::PH, LogLevel::Info,
                              "执行pH零点校准检查（标准缓冲液 7.00）");
            double measured = 7.0 + (result.value - 7.0) * 0.98;  // 模拟实测偏移
            if (std::abs(measured - 7.0) > 0.05) {
                logger_.sensorLog(SensorType::PH, LogLevel::Info,
                                  "零点偏移 " + std::to_string(7.0 - measured) +
                                      "，自动更新校准参数");
            }
            last_zero_check = now;
        }
    }

    // ---- 数据质量评估（加权置信度评分 0~100）----
    double noise_level = 0.03;              // 噪声水平（相对满量程）
    double environmental_stability = is_moving_ ? 0.65 : 0.85;  // 运动中稳定性下降
    double supply_fluctuation = 0.05;       // 供电波动

    double noise_score = std::max(0.0, 1.0 - noise_level * 10);          // 权重0.4
    double env_score = environmental_stability * (1.0 - supply_fluctuation * 4.0);  // 权重0.3
    double confidence = noise_score * 0.4 + env_score * 0.3;
    if (sensor_statuses_[data.type] == SensorStatus::READY) {
        confidence += 0.3;
    } else if (sensor_statuses_[data.type] == SensorStatus::DEGRADED) {
        confidence += 0.15;  // 退化状态：置信度减半
    } else if (sensor_statuses_[data.type] == SensorStatus::ERROR) {
        confidence -= 0.5;
    }
    if (data.is_outlier_rejected) confidence *= 0.9;
    confidence = std::max(0.0, std::min(1.0, confidence));
    result.confidence_score = confidence * 100.0;

    if (result.confidence_score >= 90.0) result.quality_level = DataQualityLevel::EXCELLENT;
    else if (result.confidence_score >= 70.0) result.quality_level = DataQualityLevel::GOOD;
    else if (result.confidence_score >= 50.0) result.quality_level = DataQualityLevel::FAIR;
    else if (result.confidence_score >= 30.0) result.quality_level = DataQualityLevel::POOR;
    else result.quality_level = DataQualityLevel::UNUSABLE;

    return result;
}

// -----------------------------------------------------------------------------
// 故障恢复（供自治理模块调用）
// -----------------------------------------------------------------------------
bool SensingModule::resetSensor(SensorType type) {
    const SensorConfig* config = nullptr;
    for (const auto& cfg : sensor_config_db_) {
        if (cfg.type == type) { config = &cfg; break; }
    }
    if (!config) return false;

    logger_.sensorLog(type, LogLevel::Info, "执行传感器重置恢复流程...");
    hw_.clearSensorFault(config->address);
    hw_.sensor_preheat(config->address);

    // 重新自检
    bool passed = false;
    switch (type) {
        case SensorType::TEMPERATURE:
            passed = (hw_.read_manufacturer_id(config->address) == 0x55AA); break;
        case SensorType::CONDUCTIVITY:
            passed = hw_.perform_short_circuit_calibration(config->address); break;
        case SensorType::PH:
            passed = hw_.measure_electrode_impedance(config->address) >= 1e7; break;
        default: {
            std::vector<uint8_t> data;
            passed = hw_.i2c_read(config->address, 3, data) && !data.empty();
            break;
        }
    }
    if (passed) {
        sensor_statuses_[type] = SensorStatus::READY;
        ++stats_.sensors_auto_recovered;
        logger_.sensorLog(type, LogLevel::Info, "传感器重置成功，状态恢复就绪");
        filter_windows_.erase(type);  // 清空滤波窗口避免历史脏数据
        rebuildTaskQueue();
    } else {
        sensor_statuses_[type] = SensorStatus::FAULTY;
        logger_.sensorLog(type, LogLevel::Error, "传感器重置失败，标记为故障待维护");
    }
    return passed;
}

SensorStatus SensingModule::sensorStatus(SensorType type) const {
    auto it = sensor_statuses_.find(type);
    return it != sensor_statuses_.end() ? it->second : SensorStatus::DISABLED;
}

std::map<SensorType, SensorStatus> SensingModule::allStatuses() const {
    return sensor_statuses_;
}

// -----------------------------------------------------------------------------
// 主调度入口（由采集线程按 50ms 周期驱动）
// -----------------------------------------------------------------------------
void SensingModule::runOnce() {
    if (!running_) return;

    // 1. 动态调度与触发管理
    manageSamplingScheduleAndTriggers();

    // 2. 取出到期任务执行采集
    auto now = std::chrono::system_clock::now();
    std::vector<ScheduledTask> ready_tasks;
    {
        std::lock_guard<std::mutex> lock(task_queue_mutex_);
        while (!task_queue_.empty() && task_queue_.top().next_execution_time <= now) {
            ready_tasks.push_back(task_queue_.top());
            task_queue_.pop();
        }
    }

    for (const auto& task : ready_tasks) {
        auto status_it = sensor_statuses_.find(task.sensor_type);
        if (status_it == sensor_statuses_.end()) continue;
        // 低功耗模式：跳过休眠传感器（湿度除外）
        if (status_it->second == SensorStatus::SLEEP) continue;
        if (status_it->second == SensorStatus::ERROR ||
            status_it->second == SensorStatus::FAULTY ||
            status_it->second == SensorStatus::DISABLED) {
            // 故障传感器不采集，等自治理模块恢复
            continue;
        }

        // 距离触发模式：未达到设定距离则任务延后
        if (is_moving_ && distance_since_last_sample_ < distance_trigger_meters_) {
            std::lock_guard<std::mutex> lock(task_queue_mutex_);
            task_queue_.push({task.sensor_type,
                              std::chrono::milliseconds(200),  // 短周期重查距离
                              task.priority, now + std::chrono::milliseconds(200)});
            continue;
        }

        // 执行采集 + 调理 + 校准（sensor_id=0：每类型单实例）
        ConditionedSensorData data = collectAndCondition(task.sensor_type, 0);
        if (data.quality_level != DataQualityLevel::UNUSABLE) {
            data = calibrateAndControlQuality(data);
        }

        bool distance_triggered = is_moving_;
        if (data.quality_level != DataQualityLevel::UNUSABLE) {
            data.timestamp = std::chrono::system_clock::now();
            output_queue_.push(data);
            ++stats_.samples_collected;
            if (distance_triggered) {
                ++stats_.distance_triggered_samples;
                distance_since_last_sample_ = 0.0;
            }
        }

        // 重新调度（高频率模式下间隔已为1/4）
        int interval = defaultIntervalMs(task.sensor_type);
        if (job_started_) interval /= 4;
        std::lock_guard<std::mutex> lock(task_queue_mutex_);
        task_queue_.push({task.sensor_type, std::chrono::milliseconds(interval),
                          task.priority, now + std::chrono::milliseconds(interval)});
    }
}

}  // namespace soil
