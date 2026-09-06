// =============================================================================
// common.h - 公共数据结构与类型定义
// 农田土壤参数移动采集系统 V1.0
//
// 本头文件统一定义系统全部模块共享的基础数据类型、枚举与时间工具。
// =============================================================================
#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <queue>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace soil {

// -----------------------------------------------------------------------------
// 系统级常量
// -----------------------------------------------------------------------------
constexpr const char* kSystemName = "农田土壤参数移动采集系统";
constexpr const char* kSystemVersion = "1.0";
constexpr const char* kDeviceId = "FSPMCS-FARM-0001";   // 设备唯一编号
constexpr const char* kFirmwareVersion = "1.0.0";

// 默认采样间隔（毫秒）：按参数变化速率与业务重要性分级调度
constexpr int kDefaultIntervalHumidityMs = 5000;    // 土壤湿度：5秒（最高优先级）
constexpr int kDefaultIntervalSoilTempMs = 30000;   // 土壤温度：30秒
constexpr int kDefaultIntervalPhMs = 120000;        // pH：2分钟
constexpr int kDefaultIntervalEcMs = 120000;        // 电导率：2分钟

// -----------------------------------------------------------------------------
// 传感器类型（统一枚举，融合 ACCELEROMETER/GYROSCOPE/GPS 供导航解算使用）
// -----------------------------------------------------------------------------
enum class SensorType {
    TEMPERATURE = 0,      // 空气/环境温度
    CONDUCTIVITY = 1,     // 土壤电导率 EC
    PH = 2,               // 土壤酸碱度
    SOIL_HUMIDITY = 3,    // 土壤体积含水率
    SOIL_TEMPERATURE = 4, // 土壤温度
    ACCELEROMETER = 5,    // 加速度计（IMU）
    GYROSCOPE = 6,        // 陀螺仪（IMU）
    GPS = 7,              // 卫星定位
    UNKNOWN = 99
};

inline const char* sensorTypeName(SensorType t) {
    switch (t) {
        case SensorType::TEMPERATURE:     return "环境温度";
        case SensorType::CONDUCTIVITY:    return "电导率";
        case SensorType::PH:              return "酸碱度";
        case SensorType::SOIL_HUMIDITY:   return "土壤湿度";
        case SensorType::SOIL_TEMPERATURE:return "土壤温度";
        case SensorType::ACCELEROMETER:   return "加速度计";
        case SensorType::GYROSCOPE:       return "陀螺仪";
        case SensorType::GPS:             return "卫星定位";
        default:                          return "未知";
    }
}

inline const char* sensorTypeCode(SensorType t) {
    switch (t) {
        case SensorType::TEMPERATURE:     return "SM100_Temp";
        case SensorType::CONDUCTIVITY:    return "SM100_EC";
        case SensorType::PH:              return "SM100_pH";
        case SensorType::SOIL_HUMIDITY:   return "SM100_VWC";
        case SensorType::SOIL_TEMPERATURE:return "SM100_STmp";
        default:                          return "UNKNOWN";
    }
}

// -----------------------------------------------------------------------------
// 传感器状态（统一枚举：合并原两处定义，含 DEGRADED/FAULTY 扩展状态）
// -----------------------------------------------------------------------------
enum class SensorStatus {
    INITIALIZING = 0,
    READY = 1,
    ERROR = 2,
    DISABLED = 3,
    SLEEP = 4,
    DEGRADED = 5,   // 性能退化：仍可用但置信度下降
    FAULTY = 6      // 已故障：退出采集，等待恢复流程
};

inline const char* sensorStatusName(SensorStatus s) {
    switch (s) {
        case SensorStatus::INITIALIZING: return "初始化中";
        case SensorStatus::READY:        return "就绪";
        case SensorStatus::ERROR:        return "错误";
        case SensorStatus::DISABLED:     return "禁用";
        case SensorStatus::SLEEP:        return "休眠";
        case SensorStatus::DEGRADED:     return "退化";
        case SensorStatus::FAULTY:       return "故障";
        default:                         return "未知";
    }
}

// -----------------------------------------------------------------------------
// 数据质量等级
// -----------------------------------------------------------------------------
enum class DataQualityLevel {
    UNUSABLE = 0,
    POOR = 1,
    FAIR = 2,
    GOOD = 3,
    EXCELLENT = 4
};

inline const char* qualityLevelName(DataQualityLevel q) {
    switch (q) {
        case DataQualityLevel::UNUSABLE:  return "不可用";
        case DataQualityLevel::POOR:      return "差";
        case DataQualityLevel::FAIR:      return "一般";
        case DataQualityLevel::GOOD:      return "良好";
        case DataQualityLevel::EXCELLENT: return "优秀";
        default:                          return "未知";
    }
}

// -----------------------------------------------------------------------------
// 传感器配置（由 config/sensors.json 加载，支持远程热更新）
// -----------------------------------------------------------------------------
struct SensorConfig {
    std::string model;                  // 传感器型号
    SensorType type = SensorType::UNKNOWN;
    int address = 0;                    // I2C/RS485 总线地址
    double reference_voltage = 2.5;     // ADC 参考电压
    int sample_rate_hz = 100;           // 采样率
    double calibration_coeffs[3] = {0.0, 1.0, 0.0};  // 二次多项式校准 y = A*x^2 + B*x + C
    int work_mode = 0;                  // 0:连续 1:触发
    int interval_ms = 60000;            // 默认采样间隔
    int priority = 5;                   // 静态采集优先级（1最高）
};

// -----------------------------------------------------------------------------
// 时空数据
// -----------------------------------------------------------------------------
struct GpsData {
    double latitude = 0.0;      // 纬度 WGS84
    double longitude = 0.0;     // 经度 WGS84
    double altitude = 0.0;      // 海拔 (m)
    double speed_mps = 0.0;     // 速度 (m/s)
    int satellite_count = 0;    // 卫星数
    std::chrono::system_clock::time_point timestamp{};
};

struct ImuData {
    double acc_x = 0, acc_y = 0, acc_z = 0;   // m/s^2
    double gyro_x = 0, gyro_y = 0, gyro_z = 0; // rad/s
    std::chrono::system_clock::time_point timestamp{};
};

struct CanSignal {
    std::string signal_name;
    int value = 0;
    std::chrono::system_clock::time_point timestamp{};
};

// -----------------------------------------------------------------------------
// 传感数据流水线结构
// -----------------------------------------------------------------------------
struct RawSensorData {
    SensorType type = SensorType::UNKNOWN;
    int sensor_id = 0;          // 同类型多传感器实例编号
    double raw_value = 0.0;     // 原始读数（电压/计数等）
    double physical_value = 0.0;// 初步转换后的物理量
    std::chrono::system_clock::time_point timestamp{};
};

struct ConditionedSensorData {
    SensorType type = SensorType::UNKNOWN;
    int sensor_id = 0;
    double value = 0.0;                 // 调理+校准后的物理量
    std::chrono::system_clock::time_point timestamp{};
    DataQualityLevel quality_level = DataQualityLevel::UNUSABLE;
    double confidence_score = 0.0;      // 0~100
    bool is_outlier_rejected = false;   // 是否发生异常值剔除
};

// 融合后的数据点（对应数据库 processed_data 表结构）
struct FusedDataPoint {
    int batch_id = 0;                   // 采集批次号
    double latitude = 0.0;              // 纬度 WGS84
    double longitude = 0.0;             // 经度 WGS84
    double local_x = 0, local_y = 0, local_z = 0;  // 局部平面坐标 (m)
    double soil_temp = 0.0;             // 土壤温度 (°C)
    double soil_moisture = 0.0;         // 土壤体积含水率 (%)
    double soil_ph = 0.0;               // pH
    double soil_ec = 0.0;               // 电导率 (uS/cm)
    double data_quality = 0.0;          // 数据质量 0~100
    std::chrono::system_clock::time_point timestamp{};
};

// 轻量级边缘分析结果
struct MLAnalysisResult {
    int soil_fertility_level = 0;     // 肥力等级 1~3
    int moisture_stress_level = 0;    // 水分胁迫等级 1~3
    int salinization_risk_level = 0;  // 盐渍化风险等级 1~3
    bool anomaly_flag = false;        // 参数异常标志
};

// -----------------------------------------------------------------------------
// 诊断日志
// -----------------------------------------------------------------------------
enum class LogLevel { Info = 0, Warning = 1, Error = 2 };  // 避免与windows.h ERROR宏冲突

struct DiagnosticLogEntry {
    std::chrono::system_clock::time_point timestamp{};
    std::string module;
    LogLevel level = LogLevel::Info;
    std::string message;
};

// -----------------------------------------------------------------------------
// 时间工具
// -----------------------------------------------------------------------------
inline int64_t toUnixMs(std::chrono::system_clock::time_point tp) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               tp.time_since_epoch()).count();
}

inline std::chrono::system_clock::time_point fromUnixMs(int64_t ms) {
    return std::chrono::system_clock::time_point(
        std::chrono::milliseconds(ms));
}

inline std::string formatTimestamp(std::chrono::system_clock::time_point tp) {
    std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm_buf{};
#if defined(_WIN32)
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  tp.time_since_epoch()).count() % 1000;
    if (ms < 0) ms += 1000;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d",
                  tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
                  static_cast<int>(ms));
    return buf;
}

inline std::string formatTimestampFull(std::chrono::system_clock::time_point tp) {
    std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm_buf{};
#if defined(_WIN32)
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif
    char buf[48];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_buf);
    return buf;
}

// 带锁安全输出（避免多线程打印交叉）
extern std::mutex g_console_mutex;
#define SCOPED_CONSOLE_LOCK() \
    std::lock_guard<std::mutex> g_console_lock_(soil::g_console_mutex)

}  // namespace soil
