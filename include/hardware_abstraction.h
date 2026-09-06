// =============================================================================
// hardware_abstraction.h - 硬件抽象层
//
// 感知与信号调理模块通过统一的设备驱动框架抽象不同接口
// （I2C/SPI/UART/SDI-12/RS-485）的传感器，提供一致的读写接口。
//
// 本层提供 SimulatedHardware 实现，用于在开发平台（Windows x86_64 /
// 通用 Linux）上仿真 i.MX8M Mini 目标硬件。部署到真实嵌入式平台时，
// 按相同接口实现 NativeHardware（Linux i2c-dev / spidev / termios /
// SocketCAN /sys/class/gpio 等），上层模块无需改动。
// =============================================================================
#pragma once

#include "common.h"

namespace soil {

// 目标硬件平台信息
struct TargetPlatformInfo {
    static constexpr const char* kCpu = "ARM Cortex-A53 四核 @1.2GHz (NXP i.MX8M Mini)";
    static constexpr const char* kMemory = "1GB LPDDR4";
    static constexpr const char* kStorage = "8GB eMMC 5.1 + 32GB microSD";
    static constexpr const char* kOs = "Linux Kernel 5.10 (PREEMPT_RT)";
};

class SimulatedHardware {
public:
    SimulatedHardware();

    // ------------------- 总线级通信原语 -------------------
    bool i2c_write(int address, const std::vector<uint8_t>& data);
    bool i2c_read(int address, int num_bytes, std::vector<uint8_t>& data);
    uint8_t spi_transfer(uint8_t data_out);
    void uart_send(const std::vector<uint8_t>& data);
    bool uart_receive(std::vector<uint8_t>& data, std::chrono::milliseconds timeout);
    double adc_read(int channel);          // 0~2.5V 参考电压

    // ------------------- 传感器功能操作 -------------------
    uint16_t read_manufacturer_id(int address);            // 数字温度传感器厂商ID
    bool perform_short_circuit_calibration(int address);   // 电导率传感器短路校准
    double measure_electrode_impedance(int address);       // pH 电极阻抗 (Ohm)
    void sensor_preheat(int address);                      // 传感器预热
    double generate_high_frequency_signal(int address);    // 100MHz 高频激励
    std::vector<uint8_t> read_sensor_registers(int address, int num_registers);

    // ------------------- 定位与运动 -------------------
    GpsData get_gps_data();                 // NMEA-0183 解析结果
    ImuData get_imu_data();                 // 加速度计 + 陀螺仪
    CanSignal receive_can_signal();         // SocketCAN 农机作业状态

    // ------------------- 场景控制（测试/演示用） -------------------
    void setVehicleMoving(bool moving);     // 设置载具运动状态
    void injectSensorFault(int address);    // 注入传感器故障（验证自恢复）
    void clearSensorFault(int address);
    bool isSensorFaultInjected(int address) const;

    // GPS 仿真参数
    void setGpsValid(bool valid) { gps_valid_ = valid; }

private:
    // 各通道的基准物理值：仿真真实农田环境（噪声+漂移+载具运动影响）
    double baseValueFor(SensorType type) const;
    // 通道地址 -> 传感器类型
    SensorType typeForAddress(int address) const;
    // 带噪声的真实感读数
    double noisyReading(SensorType type);

    mutable std::mutex mutex_;
    std::mt19937 random_engine_;
    std::map<int, SensorType> address_to_type_;

    // GPS 仿真轨迹
    double sim_lat_ = 30.486500;    // 模拟农田区域
    double sim_lon_ = 114.213000;
    double sim_speed_ = 0.0;
    bool vehicle_moving_ = false;
    double traveled_meters_ = 0.0;  // 累计行驶距离（用于距离触发）
    bool gps_valid_ = true;

    // CAN 总线仿真：周期性发送作业开始/结束
    int can_counter_ = 0;
    bool job_started_ = false;
    int job_signal_hold_ = 0;

    // 故障注入
    std::set<int> fault_injected_addresses_;

    // 漂移仿真（用于 RLS 在线校准验证）
    double slow_drift_ = 0.0;
    int drift_tick_ = 0;
};

}  // namespace soil
