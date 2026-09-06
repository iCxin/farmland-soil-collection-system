// =============================================================================
// hardware_abstraction.cpp - 硬件抽象层实现（仿真）
// =============================================================================
#include "hardware_abstraction.h"

namespace soil {

SimulatedHardware::SimulatedHardware()
    : random_engine_(std::random_device{}()) {
    // 通道地址映射（与传感器配置数据库一致）
    address_to_type_[0x10] = SensorType::TEMPERATURE;
    address_to_type_[0x11] = SensorType::CONDUCTIVITY;
    address_to_type_[0x12] = SensorType::PH;
    address_to_type_[0x13] = SensorType::SOIL_HUMIDITY;
    address_to_type_[0x14] = SensorType::SOIL_TEMPERATURE;
}

SensorType SimulatedHardware::typeForAddress(int address) const {
    auto it = address_to_type_.find(address);
    return it != address_to_type_.end() ? it->second : SensorType::UNKNOWN;
}

double SimulatedHardware::baseValueFor(SensorType type) const {
    // 典型夏季大田环境基准值
    switch (type) {
        case SensorType::TEMPERATURE:      return 28.5;   // 环境温度 °C
        case SensorType::CONDUCTIVITY:     return 850.0;  // 电导率 uS/cm
        case SensorType::PH:               return 6.8;    // pH
        case SensorType::SOIL_HUMIDITY:    return 42.0;   // 体积含水率 %
        case SensorType::SOIL_TEMPERATURE: return 24.3;   // 土壤温度 °C
        default:                           return 0.0;
    }
}

double SimulatedHardware::noisyReading(SensorType type) {
    std::lock_guard<std::mutex> lock(mutex_);
    // 慢漂移：每小时累积微小偏差，用于验证 RLS 在线校准
    if (++drift_tick_ % 50 == 0) {
        slow_drift_ += 0.002;
        if (slow_drift_ > 0.8) slow_drift_ = -0.8;  // 周期性漂移
    }
    double noise_amp = 0.0;
    switch (type) {
        case SensorType::TEMPERATURE:      noise_amp = 0.4;  break;
        case SensorType::CONDUCTIVITY:     noise_amp = 25.0; break;
        case SensorType::PH:               noise_amp = 0.05; break;
        case SensorType::SOIL_HUMIDITY:    noise_amp = 1.2;  break;
        case SensorType::SOIL_TEMPERATURE: noise_amp = 0.3;  break;
        default: break;
    }
    std::normal_distribution<double> noise(0.0, noise_amp);
    return baseValueFor(type) + slow_drift_ * 0.5 + noise(random_engine_);
}

bool SimulatedHardware::i2c_write(int address, const std::vector<uint8_t>& data) {
    (void)data;
    if (fault_injected_addresses_.count(address)) return false;
    return true;
}

bool SimulatedHardware::i2c_read(int address, int num_bytes, std::vector<uint8_t>& data) {
    if (fault_injected_addresses_.count(address)) return false;
    data.resize(static_cast<size_t>(num_bytes));
    // 将该通道的真实感物理值编码进寄存器字节（高字节在前）
    double v = noisyReading(typeForAddress(address));
    uint32_t bits;
    if (v >= 0) {
        bits = static_cast<uint32_t>(v * 100.0) & 0xFFFFFF;
    } else {
        bits = (static_cast<uint32_t>((-v) * 100.0) & 0xFFFFFF) | 0x800000;
    }
    if (num_bytes >= 1) data[0] = static_cast<uint8_t>((bits >> 16) & 0xFF);
    if (num_bytes >= 2) data[1] = static_cast<uint8_t>((bits >> 8) & 0xFF);
    if (num_bytes >= 3) data[2] = static_cast<uint8_t>(bits & 0xFF);
    for (int i = 3; i < num_bytes; ++i) data[i] = 0;
    return true;
}

uint8_t SimulatedHardware::spi_transfer(uint8_t data_out) {
    (void)data_out;
    std::uniform_int_distribution<uint8_t> dist(0, 255);
    return dist(random_engine_);
}

void SimulatedHardware::uart_send(const std::vector<uint8_t>& data) {
    (void)data;  // 仿真：不产生实际外发字节流
}

bool SimulatedHardware::uart_receive(std::vector<uint8_t>& data,
                                     std::chrono::milliseconds timeout) {
    (void)timeout;
    if (fault_injected_addresses_.count(0x10)) return false;
    double v = noisyReading(SensorType::TEMPERATURE);
    // 模拟 DHT22 帧格式：[状态][温度高][温度低][校验]
    bool negative = v < 0;
    uint16_t t10 = static_cast<uint16_t>(std::abs(v) * 10.0);
    data.resize(4);
    data[0] = 0x01;
    data[1] = static_cast<uint8_t>((t10 >> 8) & 0x7F);
    data[2] = static_cast<uint8_t>(t10 & 0xFF);
    data[3] = negative ? 0x80 : 0x00;
    return true;
}

double SimulatedHardware::adc_read(int channel) {
    SensorType t = typeForAddress(channel);
    double v = noisyReading(t);
    if (t == SensorType::PH) {
        // pH 传感器：4~20mA 变送 -> 0.4~2.0V -> pH 0~14
        double voltage = 0.4 + (v / 14.0) * 1.6;
        std::normal_distribution<double> adc_noise(0.0, 0.003);
        return voltage + adc_noise(random_engine_);
    }
    if (t == SensorType::SOIL_TEMPERATURE) {
        // 土壤温度：0.2~2.4V 对应 -10~50°C
        double voltage = 0.2 + (v + 10.0) / 60.0 * 2.2;
        std::normal_distribution<double> adc_noise(0.0, 0.002);
        return voltage + adc_noise(random_engine_);
    }
    std::uniform_real_distribution<double> dist(0.0, 2.5);
    return dist(random_engine_);
}

uint16_t SimulatedHardware::read_manufacturer_id(int address) {
    if (address == 0x10) {
        return fault_injected_addresses_.count(address) ? 0x0000 : 0x55AA;
    }
    std::uniform_int_distribution<uint16_t> dist(0, 65535);
    return dist(random_engine_);
}

bool SimulatedHardware::perform_short_circuit_calibration(int address) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));  // 仿真响应延迟
    return !fault_injected_addresses_.count(address);
}

double SimulatedHardware::measure_electrode_impedance(int address) {
    if (fault_injected_addresses_.count(address)) {
        return 5e5;  // 电极故障：阻抗异常偏低
    }
    std::uniform_real_distribution<double> dist(1e7, 8e7);  // 10M~80M Ohm 正常
    return dist(random_engine_);
}

void SimulatedHardware::sensor_preheat(int address) {
    (void)address;  // 实际平台需等待传感器内部电路稳定（50ms），仿真中省略
}

double SimulatedHardware::generate_high_frequency_signal(int address) {
    (void)address;
    std::uniform_real_distribution<double> dist(4.0, 4.6);  // 土壤介电常数 4.0~4.6
    return dist(random_engine_);
}

std::vector<uint8_t> SimulatedHardware::read_sensor_registers(int address,
                                                              int num_registers) {
    std::vector<uint8_t> data(static_cast<size_t>(num_registers), 0);
    if (fault_injected_addresses_.count(address)) return data;
    double v = noisyReading(typeForAddress(address));
    uint32_t bits = static_cast<uint32_t>(v * 100.0) & 0xFFFFFF;
    if (num_registers >= 1) data[0] = static_cast<uint8_t>((bits >> 16) & 0xFF);
    if (num_registers >= 2) data[1] = static_cast<uint8_t>((bits >> 8) & 0xFF);
    if (num_registers >= 3) data[2] = static_cast<uint8_t>(bits & 0xFF);
    return data;
}

GpsData SimulatedHardware::get_gps_data() {
    std::lock_guard<std::mutex> lock(mutex_);
    GpsData gps;
    gps.timestamp = std::chrono::system_clock::now();
    if (!gps_valid_) {
        gps.satellite_count = 0;
        gps.speed_mps = 0.0;
        return gps;
    }
    if (vehicle_moving_) {
        // 载具以 0.6~1.0 m/s 匀速前进，航向向东偏北
        std::uniform_real_distribution<double> speed_dist(0.6, 1.0);
        sim_speed_ = speed_dist(random_engine_);
        static double meters_accum = 0.0;
        // 每 tick 约行驶 sim_speed_ * 1.0s（仿真时间加速 20 倍以观察距离触发）
        meters_accum += sim_speed_ * 20.0;
        sim_lat_ += (sim_speed_ * 20.0 / 111000.0) * 0.28;   // 偏北分量
        sim_lon_ += (sim_speed_ * 20.0 / (111000.0 * std::cos(sim_lat_ * M_PI / 180.0))) * 0.96;
        traveled_meters_ = meters_accum;
        gps.satellite_count = 9;
    } else {
        sim_speed_ = 0.0;
        gps.satellite_count = 7;
    }
    std::normal_distribution<double> jitter_lat(0.0, 0.000002);
    std::normal_distribution<double> jitter_lon(0.0, 0.000002);
    gps.latitude = sim_lat_ + jitter_lat(random_engine_);
    gps.longitude = sim_lon_ + jitter_lon(random_engine_);
    gps.altitude = 23.5;
    gps.speed_mps = sim_speed_;
    return gps;
}

ImuData SimulatedHardware::get_imu_data() {
    std::lock_guard<std::mutex> lock(mutex_);
    ImuData imu;
    imu.timestamp = std::chrono::system_clock::now();
    std::normal_distribution<double> vib(0.0, vehicle_moving_ ? 0.6 : 0.05);
    imu.acc_x = vib(random_engine_);
    imu.acc_y = vib(random_engine_);
    imu.acc_z = 9.81 + vib(random_engine_) * 0.3;
    imu.gyro_x = vib(random_engine_) * 0.02;
    imu.gyro_y = vib(random_engine_) * 0.02;
    imu.gyro_z = vehicle_moving_ ? 0.01 : 0.0;
    return imu;
}

CanSignal SimulatedHardware::receive_can_signal() {
    std::lock_guard<std::mutex> lock(mutex_);
    // 仿真 CAN 报文：约每8秒产生一次作业状态切换（真实场景随农机作业节奏）
    CanSignal sig;
    sig.timestamp = std::chrono::system_clock::now();
    if (++can_counter_ % 160 == 0) {
        job_started_ = !job_started_;
        job_signal_hold_ = 5;  // 信号保持 5 次读取周期
    }
    if (job_signal_hold_ > 0) {
        --job_signal_hold_;
        sig.signal_name = job_started_ ? "JOB_START" : "JOB_END";
        sig.value = 1;
    } else {
        sig.signal_name = "NONE";
        sig.value = 0;
    }
    return sig;
}

void SimulatedHardware::setVehicleMoving(bool moving) {
    std::lock_guard<std::mutex> lock(mutex_);
    vehicle_moving_ = moving;
    if (!moving) traveled_meters_ = 0.0;
}

void SimulatedHardware::injectSensorFault(int address) {
    std::lock_guard<std::mutex> lock(mutex_);
    fault_injected_addresses_.insert(address);
}

void SimulatedHardware::clearSensorFault(int address) {
    std::lock_guard<std::mutex> lock(mutex_);
    fault_injected_addresses_.erase(address);
}

bool SimulatedHardware::isSensorFaultInjected(int address) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return fault_injected_addresses_.count(address) > 0;
}

}  // namespace soil
