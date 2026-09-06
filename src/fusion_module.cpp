// =============================================================================
// fusion_module.cpp - 模块二：智能融合与边缘分析实现
// =============================================================================
#include "fusion_module.h"
#include "logger.h"
#include "storage_module.h"

namespace soil {

namespace {

// ---------------- 小型稠密矩阵工具 ----------------
using Matrix = std::vector<std::vector<double>>;
using Vector = std::vector<double>;

Matrix multiply(const Matrix& A, const Matrix& B) {
    size_t n = A.size(), m = B[0].size(), k = B.size();
    Matrix C(n, Vector(m, 0.0));
    for (size_t i = 0; i < n; ++i)
        for (size_t p = 0; p < k; ++p) {
            if (A[i][p] == 0.0) continue;
            for (size_t j = 0; j < m; ++j) C[i][j] += A[i][p] * B[p][j];
        }
    return C;
}

Matrix transpose(const Matrix& A) {
    size_t n = A.size(), m = A[0].size();
    Matrix T(m, Vector(n, 0.0));
    for (size_t i = 0; i < n; ++i)
        for (size_t j = 0; j < m; ++j) T[j][i] = A[i][j];
    return T;
}

Matrix identity(size_t n) {
    Matrix I(n, Vector(n, 0.0));
    for (size_t i = 0; i < n; ++i) I[i][i] = 1.0;
    return I;
}

// Gauss-Jordan 求逆（仅用于低维矩阵）
Matrix inverse(const Matrix& A) {
    size_t n = A.size();
    Matrix aug(n, Vector(2 * n, 0.0));
    for (size_t i = 0; i < n; ++i) {
        for (size_t j = 0; j < n; ++j) aug[i][j] = A[i][j];
        aug[i][n + i] = 1.0;
    }
    for (size_t col = 0; col < n; ++col) {
        // 选主元
        size_t pivot = col;
        for (size_t r = col + 1; r < n; ++r) {
            if (std::abs(aug[r][col]) > std::abs(aug[pivot][col])) pivot = r;
        }
        std::swap(aug[col], aug[pivot]);
        double diag = aug[col][col];
        if (std::abs(diag) < 1e-12) diag = 1e-12;  // 防奇异
        for (size_t j = 0; j < 2 * n; ++j) aug[col][j] /= diag;
        for (size_t r = 0; r < n; ++r) {
            if (r == col) continue;
            double factor = aug[r][col];
            if (factor == 0.0) continue;
            for (size_t j = 0; j < 2 * n; ++j) aug[r][j] -= factor * aug[col][j];
        }
    }
    Matrix inv(n, Vector(n, 0.0));
    for (size_t i = 0; i < n; ++i)
        for (size_t j = 0; j < n; ++j) inv[i][j] = aug[i][n + j];
    return inv;
}

// 状态索引：0温度 1水分 2电导率 3饱和度 4pH 5氧化还原电位 6有机质 7容重
constexpr int kIdxTemp = 0, kIdxMoisture = 1, kIdxEc = 2, kIdxSaturation = 3,
              kIdxPh = 4, kIdxRedox = 5, kIdxOm = 6, kIdxBulkDensity = 7;

// 默认系统噪声协方差 Q（对角）
std::array<double, 8> defaultQ() {
    return {0.01, 0.005, 0.05, 0.01, 0.002, 1.0, 0.001, 0.0005};
}

}  // namespace

// -----------------------------------------------------------------------------
FusionModule::FusionModule(SimulatedHardware& hw,
                           ThreadSafeQueue<FusedRecord>& output_queue,
                           StorageModule& storage,
                           Logger& logger)
    : hw_(hw), output_queue_(output_queue), storage_(storage), logger_(logger) {
    last_imu_time_ = std::chrono::system_clock::now();
}

int FusionModule::kalmanIndexFor(SensorType type) {
    switch (type) {
        case SensorType::SOIL_TEMPERATURE: return kIdxTemp;
        case SensorType::SOIL_HUMIDITY:    return kIdxMoisture;
        case SensorType::CONDUCTIVITY:     return kIdxEc;
        case SensorType::PH:               return kIdxPh;
        default:                           return -1;
    }
}

// 状态向量内部单位与物理量纲一致（温度°C、水分%、EC uS/cm、pH无量纲）
double FusionModule::stateToPhysical(int /*index*/, double value) { return value; }
double FusionModule::physicalToState(int /*index*/, double value) { return value; }

bool FusionModule::initialize() {
    // 卡尔曼初始状态
    kalman_state_.state = {24.0, 40.0, 800.0, 0.45, 6.5, 400.0, 2.0, 1.4};
    kalman_state_.covariance = {};
    const std::array<double, 8> init_var = {1.0, 0.04, 100.0, 0.04, 0.01, 100.0, 0.1, 0.01};
    for (int i = 0; i < kNumStateVars; ++i) {
        kalman_state_.covariance[i][i] = init_var[i];
    }
    kalman_state_.state_timestamp = std::chrono::system_clock::now();
    kalman_state_.initialized = true;

    // 轻量级 ML 模型参数：优先从持久化模块加载（OTA 更新通道），否则使用默认权重
    ml_model_params_ = storage_.loadMlModelParams();
    if (ml_model_params_.size() < static_cast<size_t>(3 * kMlFeatureDim)) {
        ml_model_params_.assign(static_cast<size_t>(3 * kMlFeatureDim), 0.0);
        // 类别0（肥力）：EC/水分比值与有机质主导
        for (int i = 0; i < kMlFeatureDim; ++i) ml_model_params_[i] = 0.18;
        // 类别1（水分胁迫）：水分相关特征主导
        ml_model_params_[kMlFeatureDim + 1] = 0.30;
        ml_model_params_[kMlFeatureDim + 4] = 0.12;
        // 类别2（盐渍化风险）：EC 相关特征主导
        ml_model_params_[2 * kMlFeatureDim + 0] = 0.28;
        ml_model_params_[2 * kMlFeatureDim + 2] = 0.10;
        storage_.saveMlModelParams(ml_model_params_);
    }

    pps_base_time_ = std::chrono::system_clock::now();
    logger_.log("融合", LogLevel::Info,
                "智能融合与边缘分析模块初始化完成（8状态卡尔曼 + RLS在线校准 + 轻量级ML）");
    return true;
}

// -----------------------------------------------------------------------------
// 多源数据时空对齐与预处理
// -----------------------------------------------------------------------------
void FusionModule::syncClockWithPps() {
    // GPS PPS 秒脉冲同步，消除本地时钟漂移（微秒级精度）
    pps_base_time_ = std::chrono::system_clock::now();
}

double FusionModule::lagrangeInterpolate(
    const std::vector<std::pair<std::chrono::system_clock::time_point, double>>& points,
    std::chrono::system_clock::time_point target) {
    if (points.size() < 3) {
        if (points.empty()) return NAN;
        if (points.size() == 1) return points[0].second;
        // 两点线性插值
        auto dt_total = points[1].first - points[0].first;
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(dt_total).count();
        if (ms == 0) return points[0].second;
        double alpha = std::chrono::duration<double>(target - points[0].first).count() /
                       std::chrono::duration<double>(dt_total).count();
        alpha = std::max(0.0, std::min(1.0, alpha));
        return points[0].second * (1.0 - alpha) + points[1].second * alpha;
    }
    // 取时间上最接近的3个点做拉格朗日三点插值
    std::vector<std::pair<std::chrono::system_clock::time_point, double>> closest = points;
    std::sort(closest.begin(), closest.end(),
              [&target](const auto& a, const auto& b) {
                  return std::abs(std::chrono::duration_cast<std::chrono::microseconds>(
                                      a.first - target).count()) <
                         std::abs(std::chrono::duration_cast<std::chrono::microseconds>(
                                      b.first - target).count());
              });
    closest.resize(3);
    double t0 = static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(
                    closest[0].first - target).count());
    double t1 = static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(
                    closest[1].first - target).count());
    double t2 = static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(
                    closest[2].first - target).count());
    double y0 = closest[0].second, y1 = closest[1].second, y2 = closest[2].second;
    double denom0 = (t0 - t1) * (t0 - t2);
    double denom1 = (t1 - t0) * (t1 - t2);
    double denom2 = (t2 - t0) * (t2 - t1);
    if (std::abs(denom0) < 1e-9 || std::abs(denom1) < 1e-9 || std::abs(denom2) < 1e-9) {
        return y0;
    }
    return y0 * (t1 * t2) / denom0 + y1 * (t0 * t2) / denom1 + y2 * (t0 * t1) / denom2;
}

std::chrono::milliseconds FusionModule::interpolationWindow(SensorType type) const {
    switch (type) {
        case SensorType::PH:              return std::chrono::seconds(30);
        case SensorType::SOIL_TEMPERATURE:return std::chrono::seconds(10);
        case SensorType::SOIL_HUMIDITY:   return std::chrono::seconds(15);
        case SensorType::CONDUCTIVITY:    return std::chrono::seconds(20);
        default:                          return std::chrono::seconds(10);
    }
}

void FusionModule::updateIns(const ImuData& imu, const GpsData& gps) {
    auto now = imu.timestamp;
    double dt = std::chrono::duration<double>(now - last_imu_time_).count();
    if (dt <= 0 || dt > 10.0) dt = 0.05;

    if (gps.satellite_count >= 4 && gps.speed_mps > 0.01) {
        // GPS 有效：以 GPS 修正 INS（简化：位置重置到 GPS 平面投影）
        if (!local_origin_set_) {
            origin_lat_ = gps.latitude;
            origin_lon_ = gps.longitude;
            local_origin_set_ = true;
        }
        double lat_diff = (gps.latitude - origin_lat_) * 111000.0;
        double lon_diff = (gps.longitude - origin_lon_) * 111000.0 *
                          std::cos(origin_lat_ * M_PI / 180.0);
        ins_position_ = {lon_diff, lat_diff, gps.altitude};
    } else {
        // GPS 失效：IMU 惯性推算（已做零偏校正，二次积分）
        ins_position_[0] += 0.5 * imu.acc_x * dt * dt;
        ins_position_[1] += 0.5 * imu.acc_y * dt * dt;
        ins_position_[2] += 0.5 * (imu.acc_z - 9.81) * dt * dt;
    }
    last_imu_time_ = now;
    last_lat_ = gps.latitude;
    last_lon_ = gps.longitude;
}

// -----------------------------------------------------------------------------
// RLS 递推最小二乘在线校准
// -----------------------------------------------------------------------------
void FusionModule::rlsUpdate(SensorType type, double measured, double reference) {
    RlsParams& p = rls_params_[type];
    // 回归向量 phi = [1, reference]，模型 measured = theta0 + theta1 * reference
    double phi[2] = {1.0, reference};
    // 增益向量 K = P*phi / (lambda + phi^T*P*phi)
    double Pphi[2] = {p.P[0][0] * phi[0] + p.P[0][1] * phi[1],
                      p.P[1][0] * phi[0] + p.P[1][1] * phi[1]};
    double denom = kRlsLambda + phi[0] * Pphi[0] + phi[1] * Pphi[1];
    if (std::abs(denom) < 1e-9) return;
    double K[2] = {Pphi[0] / denom, Pphi[1] / denom};
    // 先验残差
    double y_hat = p.theta0 * phi[0] + p.theta1 * phi[1];
    double e = measured - y_hat;
    // 参数更新
    p.theta0 += K[0] * e;
    p.theta1 += K[1] * e;
    // 协方差更新 P = (P - K*phi^T*P) / lambda
    double Pnew[2][2];
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 2; ++j) {
            double phiP_j = phi[0] * p.P[0][j] + phi[1] * p.P[1][j];
            Pnew[i][j] = (p.P[i][j] - K[i] * phiP_j) / kRlsLambda;
        }
    p.P[0][0] = Pnew[0][0]; p.P[0][1] = Pnew[0][1];
    p.P[1][0] = Pnew[1][0]; p.P[1][1] = Pnew[1][1];
    ++p.update_count;
}

// -----------------------------------------------------------------------------
// 数据提交与健康检查
// -----------------------------------------------------------------------------
void FusionModule::submitConditioned(const ConditionedSensorData& data) {
    if (data.quality_level == DataQualityLevel::UNUSABLE) return;

    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        pending_raws_.push_back(data);   // 样本快照随下一融合点持久化
    }
    // 物理量合理性检查（规则式异常检测）
    bool plausible = true;
    switch (data.type) {
        case SensorType::SOIL_HUMIDITY:    plausible = data.value > 0.0 && data.value < 70.0; break;
        case SensorType::SOIL_TEMPERATURE: plausible = data.value > -10.0 && data.value < 60.0; break;
        case SensorType::TEMPERATURE:      plausible = data.value > -20.0 && data.value < 70.0; break;
        case SensorType::PH:               plausible = data.value > 3.0 && data.value < 10.0; break;
        case SensorType::CONDUCTIVITY:     plausible = data.value > 0.0 && data.value < 5000.0; break;
        default: break;
    }
    if (!plausible) {
        logger_.sensorLog(data.type, LogLevel::Warning,
                          "融合前物理量合理性检查未通过: " + std::to_string(data.value) +
                              "，丢弃该样本");
        return;
    }

    // RLS 在线校准：以当前融合估计为参考修正慢漂移（测量 = theta0 + theta1*真值）
    int idx = kalmanIndexFor(data.type);
    if (idx >= 0 && kalman_state_.initialized) {
        rlsUpdate(data.type, data.value,
                  physicalToState(idx, kalman_state_.state[idx]));
        const RlsParams& p = rls_params_[data.type];
        if (p.update_count > 10) {
            // 应用校准逆变换
            double corrected = (data.value - p.theta0) / (std::abs(p.theta1) < 0.05 ? 0.05 : p.theta1);
            ConditionedSensorData corrected_data = data;
            corrected_data.value = corrected;
            std::lock_guard<std::mutex> lock(buffer_mutex_);
            auto& buf = reading_buffer_[data.type];
            buf.push_back({corrected_data.timestamp, corrected_data.value});
            while (buf.size() > kMaxBufferedPerType) buf.erase(buf.begin());
            return;
        }
    }

    std::lock_guard<std::mutex> lock(buffer_mutex_);
    auto& buf = reading_buffer_[data.type];
    buf.push_back({data.timestamp, data.value});
    while (buf.size() > kMaxBufferedPerType) buf.erase(buf.begin());
}

void FusionModule::checkSensorHealth(const ConditionedSensorData& data) {
    // 交叉验证：电导率与水分正相关一致性
    if (data.type == SensorType::CONDUCTIVITY) {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        auto it = reading_buffer_.find(SensorType::SOIL_HUMIDITY);
        if (it != reading_buffer_.end() && !it->second.empty()) {
            double moisture = it->second.back().second;
            // 简化规则：含水率>20% 时电导率不应接近 0（探头悬空/接触不良）
            if (moisture > 20.0 && data.value < 50.0) {
                logger_.sensorLog(SensorType::CONDUCTIVITY, LogLevel::Warning,
                                  "交叉验证异常: 含水率 " + std::to_string(moisture) +
                                      "% 与电导率 " + std::to_string(data.value) +
                                      " uS/cm 不一致，标记传感器退化");
            }
        }
    }
}

// -----------------------------------------------------------------------------
// 基于卡尔曼滤波的动态数据融合
// -----------------------------------------------------------------------------
void FusionModule::kalmanPredict(double dt, bool vibrating) {
    // 状态转移矩阵 F（含跨参数耦合：EC受水分影响、饱和度依赖水分）
    Matrix F = identity(kNumStateVars);
    F[kIdxTemp][kIdxTemp] = std::exp(-dt / 3600.0);           // 温度向环境温度衰减
    F[kIdxMoisture][kIdxMoisture] = std::exp(-dt / 7200.0);   // 水分运移
    F[kIdxEc][kIdxMoisture] = 0.1;
    F[kIdxEc][kIdxEc] = std::exp(-dt / 5400.0);
    F[kIdxSaturation][kIdxMoisture] = 0.2;
    F[kIdxSaturation][kIdxSaturation] = std::exp(-dt / 10800.0);
    F[kIdxPh][kIdxPh] = std::exp(-dt / 86400.0);              // pH 缓慢变化
    F[kIdxRedox][kIdxRedox] = std::exp(-dt / 43200.0);
    F[kIdxOm][kIdxOm] = 0.9999;                                // 有机质季节尺度
    F[kIdxBulkDensity][kIdxBulkDensity] = 0.9999;

    // 预测状态 x = F*x
    std::array<double, 8> x_pred{};
    for (int i = 0; i < kNumStateVars; ++i) {
        double acc = 0.0;
        for (int j = 0; j < kNumStateVars; ++j) acc += F[i][j] * kalman_state_.state[j];
        x_pred[i] = acc;
    }

    // 自适应系统噪声 Q（振动环境下增大水分/容重过程噪声）
    std::array<double, 8> q = defaultQ();
    if (vibrating) {
        q[kIdxMoisture] += 0.005;
        q[kIdxBulkDensity] += 0.002;
    }

    // 预测协方差 P = F*P*F^T + Q
    Matrix P(kNumStateVars, Vector(kNumStateVars, 0.0));
    for (int i = 0; i < kNumStateVars; ++i)
        for (int j = 0; j < kNumStateVars; ++j)
            P[i][j] = kalman_state_.covariance[i][j];
    Matrix P_pred = multiply(multiply(F, P), transpose(F));
    for (int i = 0; i < kNumStateVars; ++i) {
        for (int j = 0; j < kNumStateVars; ++j) {
            kalman_state_.covariance[i][j] = P_pred[i][j];
        }
        kalman_state_.covariance[i][i] += q[i];
    }
    kalman_state_.state = x_pred;
}

void FusionModule::kalmanUpdate(const std::map<int, double>& measured_index_to_value,
                                const std::map<int, double>& measured_index_to_variance,
                                const std::chrono::system_clock::time_point& stamp) {
    size_t m = measured_index_to_value.size();
    if (m == 0) return;

    // 测量矩阵 H (m x 8)：每个测量直接观测对应状态分量
    Matrix H(m, Vector(kNumStateVars, 0.0));
    size_t row = 0;
    for (const auto& kv : measured_index_to_value) {
        H[row][kv.first] = 1.0;
        ++row;
    }
    // 测量向量 z 与测量噪声对角阵 R
    Vector z(m);
    Matrix R(m, Vector(m, 0.0));
    row = 0;
    for (const auto& kv : measured_index_to_value) {
        z[row] = kv.second;
        double var = 1.0;
        auto vit = measured_index_to_variance.find(kv.first);
        if (vit != measured_index_to_variance.end()) var = vit->second;
        R[row][row] = var;
        ++row;
    }

    Matrix P(kNumStateVars, Vector(kNumStateVars, 0.0));
    for (int i = 0; i < kNumStateVars; ++i)
        for (int j = 0; j < kNumStateVars; ++j)
            P[i][j] = kalman_state_.covariance[i][j];

    // S = H*P*H^T + R
    Matrix S = multiply(multiply(H, P), transpose(H));
    for (size_t i = 0; i < m; ++i)
        for (size_t j = 0; j < m; ++j) S[i][j] += R[i][j];

    // K = P*H^T*S^-1
    Matrix K = multiply(multiply(P, transpose(H)), inverse(S));

    // 新息 y = z - H*x
    Vector innovation(m, 0.0);
    for (size_t i = 0; i < m; ++i) {
        double hx = 0.0;
        for (int j = 0; j < kNumStateVars; ++j) hx += H[i][j] * kalman_state_.state[j];
        innovation[i] = z[i] - hx;
    }

    // 状态更新 x = x + K*y
    for (int i = 0; i < kNumStateVars; ++i) {
        double delta = 0.0;
        for (size_t j = 0; j < m; ++j) delta += K[i][j] * innovation[j];
        kalman_state_.state[i] += delta;
    }

    // 协方差更新 P = (I - K*H)*P（Joseph 简化形式）
    Matrix KH = multiply(K, H);
    Matrix IKH = identity(kNumStateVars);
    for (int i = 0; i < kNumStateVars; ++i)
        for (int j = 0; j < kNumStateVars; ++j) IKH[i][j] -= KH[i][j];
    Matrix P_new = multiply(IKH, P);
    for (int i = 0; i < kNumStateVars; ++i)
        for (int j = 0; j < kNumStateVars; ++j) {
            kalman_state_.covariance[i][j] = P_new[i][j];
        }

    kalman_state_.state_timestamp = stamp;
}

// -----------------------------------------------------------------------------
// 轻量级机器学习分析
// -----------------------------------------------------------------------------
MLAnalysisResult FusionModule::performLightweightAnalysis(const KalmanState& ks) {
    // 16维特征工程：原始状态 + 派生比值特征
    std::vector<double> feat(kMlFeatureDim, 0.0);
    double moisture = std::max(1.0, ks.state[kIdxMoisture]);
    feat[0] = ks.state[kIdxEc] / moisture;        // EC/水分比（盐分富集指标）
    feat[1] = moisture / 100.0;
    feat[2] = ks.state[kIdxEc] / 1000.0;
    feat[3] = ks.state[kIdxPh] / 14.0;
    feat[4] = ks.state[kIdxTemp] / 50.0;
    feat[5] = ks.state[kIdxOm] / 10.0;
    feat[6] = ks.state[kIdxSaturation];
    feat[7] = ks.state[kIdxRedox] / 1000.0;
    feat[8] = ks.state[kIdxBulkDensity] / 3.0;
    feat[9] = feat[0] * feat[3];                  // 交互特征
    feat[10] = feat[1] * feat[4];
    feat[11] = ks.state[kIdxEc] > 2000.0 ? 1.0 : 0.0;   // 离散化：高盐
    feat[12] = ks.state[kIdxPh] < 5.5 ? 1.0 : 0.0;      // 离散化：酸化
    feat[13] = ks.state[kIdxMoisture] < 15.0 ? 1.0 : 0.0; // 离散化：干旱
    feat[14] = ks.state[kIdxOm] < 1.0 ? 1.0 : 0.0;      // 离散化：贫瘠
    feat[15] = 1.0;                               // 偏置项

    // 线性评分模型（3类）
    std::vector<double> scores(3, 0.0);
    for (int c = 0; c < 3; ++c) {
        for (int i = 0; i < kMlFeatureDim; ++i) {
            scores[c] += feat[i] * ml_model_params_[static_cast<size_t>(c * kMlFeatureDim + i)];
        }
    }

    // 决策树式阈值判别（固定深度，适合嵌入式推理）
    MLAnalysisResult result;
    result.soil_fertility_level =
        (scores[0] > 2.5) ? 3 : (scores[0] > 1.2 ? 2 : 1);
    result.moisture_stress_level =
        (ks.state[kIdxMoisture] < 12.0) ? 3 : (ks.state[kIdxMoisture] < 22.0 ? 2 : 1);
    result.salinization_risk_level =
        (ks.state[kIdxEc] > 2500.0) ? 3 : (ks.state[kIdxEc] > 1200.0 ? 2 : 1);
    result.anomaly_flag =
        ks.state[kIdxPh] < 4.0 || ks.state[kIdxPh] > 9.5 ||
        ks.state[kIdxEc] > 4000.0 || ks.state[kIdxMoisture] < 5.0 ||
        ks.state[kIdxTemp] < 0.0 || ks.state[kIdxTemp] > 45.0;
    return result;
}

// -----------------------------------------------------------------------------
// 传感器故障时的缺失值估算
// -----------------------------------------------------------------------------
double FusionModule::estimateMissingValue(SensorType missing_type) const {
    // 基于历史数据训练的回归模型（简化版）：从关联传感器估算
    const auto& ks = kalman_state_;
    switch (missing_type) {
        case SensorType::PH:
            // pH 与 EC/有机质相关模型（训练样本拟合）
            if (ks.state[kIdxEc] > 0) {
                return 7.2 - 0.0002 * ks.state[kIdxEc] - 0.05 * (ks.state[kIdxTemp] - 25.0);
            }
            return 7.0;
        case SensorType::SOIL_HUMIDITY:
            // 由电导率粗估含水率
            return std::max(5.0, std::min(60.0, ks.state[kIdxEc] / 30.0));
        case SensorType::CONDUCTIVITY:
            // 由含水率粗估电导率
            return std::max(50.0, ks.state[kIdxMoisture] * 30.0);
        case SensorType::SOIL_TEMPERATURE:
            // 土壤温度跟随气温（滞后）
            return ks.state[kIdxTemp];
        default:
            return NAN;
    }
}

// -----------------------------------------------------------------------------
// 融合主循环（由数据处理线程周期驱动）
// -----------------------------------------------------------------------------
bool FusionModule::runFusionCycle() {
    // 1. 时钟同步 + INS 更新
    syncClockWithPps();
    GpsData gps = hw_.get_gps_data();
    ImuData imu = hw_.get_imu_data();
    updateIns(imu, gps);
    bool vibrating = std::abs(imu.acc_x) > 0.3 || std::abs(imu.acc_y) > 0.3;
    bool used_dead_reckoning = gps.satellite_count < 4;

    // 2. 时空对齐：将各传感器缓冲数据插值到统一融合时刻
    auto target_time = std::chrono::system_clock::now();
    std::map<int, double> measurements;     // 卡尔曼状态索引 -> 插值测量值
    std::map<int, double> variances;        // 卡尔曼状态索引 -> 测量噪声方差

    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        for (auto const& [type, points] : reading_buffer_) {
            if (points.empty()) continue;
            auto window = interpolationWindow(type);
            std::vector<std::pair<std::chrono::system_clock::time_point, double>> relevant;
            for (const auto& p : points) {
                if (target_time - p.first <= window) relevant.push_back(p);
            }
            if (relevant.empty()) continue;
            double value = (relevant.size() >= 3)
                               ? lagrangeInterpolate(relevant, target_time)
                               : relevant.back().second;
            if (std::isnan(value)) continue;
            int idx = kalmanIndexFor(type);
            if (idx >= 0) {
                measurements[idx] = physicalToState(idx, value);
                // 测量噪声方差与传感器质量挂钩
                double var = 1.0;
                switch (type) {
                    case SensorType::SOIL_TEMPERATURE: var = 0.25; break;
                    case SensorType::SOIL_HUMIDITY:    var = 1.0;  break;
                    case SensorType::CONDUCTIVITY:     var = 100.0; break;
                    case SensorType::PH:               var = 0.01; break;
                    default: break;
                }
                variances[idx] = var;
            }
        }
    }

    // 3. 缺失参数估算补全（传感器故障/休眠/采样周期过长时，
    //    使用关联传感器回归模型估算缺失值，并赋较大测量方差）
    std::vector<std::string> estimated_fields;
    const std::vector<std::pair<SensorType, int>> measured_types = {
        {SensorType::SOIL_TEMPERATURE, kIdxTemp},
        {SensorType::SOIL_HUMIDITY, kIdxMoisture},
        {SensorType::CONDUCTIVITY, kIdxEc},
        {SensorType::PH, kIdxPh},
    };
    for (const auto& [type, idx] : measured_types) {
        if (measurements.count(idx)) continue;
        double est = estimateMissingValue(type);
        if (!std::isnan(est)) {
            measurements[idx] = est;
            variances[idx] = 4.0;   // 估算值：较大测量方差
            estimated_fields.push_back(sensorTypeName(type));
            ++estimated_values_count_;
        }
    }
    bool ph_estimated =
        std::find(estimated_fields.begin(), estimated_fields.end(),
                  sensorTypeName(SensorType::PH)) != estimated_fields.end();

    // 4. 卡尔曼预测 + 更新
    double dt = std::chrono::duration<double>(target_time - kalman_state_.state_timestamp).count();
    dt = std::max(0.001, std::min(dt, 3600.0));
    kalmanPredict(dt, vibrating);
    kalmanUpdate(measurements, variances, target_time);

    // 5. 构造融合输出
    FusedRecord record;
    record.point.batch_id = ++batch_counter_;
    record.point.timestamp = target_time;
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        record.contributing_raws = std::move(pending_raws_);
        pending_raws_.clear();
    }
    record.point.latitude = gps.latitude;
    record.point.longitude = gps.longitude;
    record.point.local_x = ins_position_[0];
    record.point.local_y = ins_position_[1];
    record.point.local_z = ins_position_[2];
    record.point.soil_temp = kalman_state_.state[kIdxTemp];
    record.point.soil_moisture = kalman_state_.state[kIdxMoisture];
    record.point.soil_ec = kalman_state_.state[kIdxEc];
    record.point.soil_ph = kalman_state_.state[kIdxPh];
    record.used_ins_dead_reckoning = used_dead_reckoning;
    record.ph_estimated = ph_estimated;
    record.estimated_fields = estimated_fields;

    // 6. 轻量级边缘分析
    record.analysis = performLightweightAnalysis(kalman_state_);

    // 7. 数据质量评分：融合后状态协方差 + 有效测量占比
    double measured_ratio = static_cast<double>(measurements.size()) / 4.0;
    double trace = 0.0;
    for (int i = 0; i < 4; ++i) trace += kalman_state_.covariance[i][i];
    double cov_score = std::max(0.0, 1.0 - trace / 50.0);
    record.point.data_quality = std::max(0.0, std::min(100.0,
        (cov_score * 0.5 + measured_ratio * 0.5) * 100.0));

    // 8. 输出到数据组织与持久化模块
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        last_point_ = record.point;
        has_last_point_ = true;
    }
    output_queue_.push(record);
    ++fused_count_;
    return true;
}

}  // namespace soil
