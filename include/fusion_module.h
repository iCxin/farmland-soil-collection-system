// =============================================================================
// fusion_module.h - 模块二：智能融合与边缘分析
//
// 职责：
//   1. 多源数据时空对齐与预处理（GPS PPS高精度时钟 / INS惯性推算 / 拉格朗日插值）
//   2. 基于扩展卡尔曼滤波(EKF)的8状态动态数据融合（自适应过程噪声）
//   3. 递推最小二乘(RLS)在线校准：补偿传感器慢变漂移
//   4. 轻量级边缘分析：肥力分级 / 水分胁迫 / 盐渍化风险 / 参数异常判定
// =============================================================================
#pragma once

#include "common.h"
#include "data_queue.h"
#include "hardware_abstraction.h"

#include <array>

namespace soil {

class Logger;
class StorageModule;

// 融合输出记录：数据点 + 边缘分析结果
struct FusedRecord {
    FusedDataPoint point;
    MLAnalysisResult analysis;
    std::vector<ConditionedSensorData> contributing_raws;  // 本批次参与融合的调理样本快照
    std::vector<std::string> estimated_fields;             // 估算补全的参数名（故障降级）
    bool ph_estimated = false;      // pH 是否为估算补全值
    bool used_ins_dead_reckoning = false;
};

// 卡尔曼滤波器状态向量（8变量）
struct KalmanState {
    std::array<double, 8> state{};                 // Temp, Moisture, EC, Sat, pH, Redox, OM, BulkDensity
    std::array<std::array<double, 8>, 8> covariance{};
    std::chrono::system_clock::time_point state_timestamp{};
    bool initialized = false;
};

class FusionModule {
public:
    FusionModule(SimulatedHardware& hw,
                 ThreadSafeQueue<FusedRecord>& output_queue,
                 StorageModule& storage,
                 Logger& logger);

    bool initialize();

    // 数据处理线程调用：接收感知模块输出
    void submitConditioned(const ConditionedSensorData& data);

    // 周期驱动：执行时空对齐 -> 卡尔曼融合 -> 在线校准 -> 轻量级分析
    // 返回 true 表示本轮产生了融合数据点
    bool runFusionCycle();

    // 传感器故障时用其他关联传感器估算缺失值（降级运行）
    double estimateMissingValue(SensorType missing_type) const;

    // 传感器一致性/交叉验证健康检查
    void checkSensorHealth(const ConditionedSensorData& data);

    const KalmanState& kalmanState() const { return kalman_state_; }
    int fused_count() const { return fused_count_; }
    FusedDataPoint lastPoint() const {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        return last_point_;
    }
    bool hasLastPoint() const { return has_last_point_; }
    double last_latitude() const { return last_lat_; }
    double last_longitude() const { return last_lon_; }

private:
    // - 时空对齐 ----
    static double lagrangeInterpolate(
        const std::vector<std::pair<std::chrono::system_clock::time_point, double>>& points,
        std::chrono::system_clock::time_point target);
    std::chrono::milliseconds interpolationWindow(SensorType type) const;
    void syncClockWithPps();

    // ---- INS ----
    void updateIns(const ImuData& imu, const GpsData& gps);
    std::array<double, 3> localPosition() const;

    // - 卡尔曼滤波（正确矩阵实现） ----
    void kalmanPredict(double dt, bool vibrating);
    void kalmanUpdate(const std::map<int, double>& measured_index_to_value,
                      const std::map<int, double>& measured_index_to_variance,
                      const std::chrono::system_clock::time_point& stamp);

    // - RLS 在线校准 ----
    void rlsUpdate(SensorType type, double measured, double reference);

    // - 轻量级分析 ----
    MLAnalysisResult performLightweightAnalysis(const KalmanState& ks);

    // 物理量 <-> 卡尔曼状态索引映射
    static int kalmanIndexFor(SensorType type);
    static double stateToPhysical(int index, double value);
    static double physicalToState(int index, double value);

    SimulatedHardware& hw_;
    ThreadSafeQueue<FusedRecord>& output_queue_;
    StorageModule& storage_;
    Logger& logger_;

    // 读数缓冲（供插值）
    std::map<SensorType, std::vector<std::pair<std::chrono::system_clock::time_point, double>>> reading_buffer_;
    std::vector<ConditionedSensorData> pending_raws_;   // 待随融合点落盘的样本快照
    mutable std::mutex buffer_mutex_;   // lastPoint() 为 const 接口
    static constexpr size_t kMaxBufferedPerType = 64;

    // 高精度时钟（GPS PPS 同步）
    std::chrono::system_clock::time_point pps_base_time_;

    // INS 状态
    std::array<double, 3> ins_position_{{0.0, 0.0, 23.5}};
    std::chrono::system_clock::time_point last_imu_time_;
    bool local_origin_set_ = false;
    double origin_lat_ = 0.0, origin_lon_ = 0.0;
    double last_lat_ = 0.0, last_lon_ = 0.0;

    // 卡尔曼滤波器
    KalmanState kalman_state_;
    static constexpr int kNumStateVars = 8;

    // RLS 在线校准参数：theta = [theta0, theta1]，measured ≈ theta0 + theta1*true
    struct RlsParams {
        double theta0 = 0.0;      // 漂移偏置估计
        double theta1 = 1.0;      // 尺度因子估计
        double P[2][2] = {{100.0, 0.0}, {0.0, 100.0}};  // 参数协方差
        int update_count = 0;
    };
    std::map<SensorType, RlsParams> rls_params_;
    static constexpr double kRlsLambda = 0.995;  // 遗忘因子
    static constexpr double kRlsNoiseVar = 0.01;

    // 轻量级 ML 模型参数（3类 x 16维，通过OTA可更新）
    std::vector<double> ml_model_params_;
    static constexpr int kMlFeatureDim = 16;

    int batch_counter_ = 0;
    int fused_count_ = 0;
    FusedDataPoint last_point_;      // 最新融合点（供 Web 面板快照）
    bool has_last_point_ = false;
    int estimated_values_count_ = 0;
    int last_known_saturation_ = 0;  // 状态索引
};

}  // namespace soil
