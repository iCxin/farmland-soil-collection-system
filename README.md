<div align="right"><sub><code>iCxin / farmland-soil-collection-system</code></sub></div>

# 农田土壤参数移动采集系统（FSPMCS）

> 面向精准农业的嵌入式边缘采集终端：多传感器融合、结构化落盘、Web 实时监控。
> An embedded edge terminal for precision agriculture: sensor fusion, structured storage, live web dashboard.

| | |
|:--|:--|
| **状态** | 活跃维护 |
| **栈** | `C++17` · 标准库（零第三方依赖） |
| **平台** | Windows · Linux（目标机：ARM Cortex-A53 + Linux 5.10） |
| **许可** | [MIT](LICENSE) |

---

## 01 · 这是什么

集成多参数土壤传感器，实时同步采集温度、湿度、pH、电导率；在边缘侧完成多传感器融合、自适应校准与轻量分析，输出结构化数据。系统支持农机、巡检机器人等移动载具部署，并内嵌开箱即用的 Web 实时监控面板。系统编排器下分感知与信号调理、智能融合与边缘分析、数据组织与持久化、通信与协同、系统自治理五个模块，由 6 个核心线程（主监控 / 数据采集 / 数据处理 / 存储转发 / 网络监听 / 定时任务）驱动。

纯 C++17 与标准库实现，零第三方依赖，单二进制交付，Windows / Linux 双平台编译运行。开发阶段通过 `SimulatedHardware` 仿真硬件，部署到 i.MX8M Mini 等目标机时按同一接口实现 `NativeHardware` 即可。

## 02 · 功能特性

- **感知与信号调理**：I2C / SPI / UART / ADC 多总线接入（含 SDI-12、RS-485 扩展位）；上电自检含厂商 ID 校验、电导率短路校准、pH 电极阻抗测量；优先级任务队列调度，CAN 作业信号触发高频模式（采样间隔缩至 1/4）；位置感知每 2 米采样，静止 10 分钟进入低功耗监测。
- **三级信号调理与质量控制**：滑动平均滤波 → Tukey 围栏异常值剔除 → 传感器特异性温度补偿；二次多项式校准、pH 凌晨零点校准、0~100 加权置信度评分。
- **智能融合与边缘分析**：8 状态扩展卡尔曼滤波（过程噪声随载具振动自适应）、RLS 在线校准补偿慢变漂移；GPS PPS + INS 惯性推算，拉格朗日三点插值时空对齐；肥力等级 / 水分胁迫 / 盐渍化风险分级与参数异常判定；传感器故障时按关联模型估算缺失参数，降级运行。
- **数据组织与持久化**：嵌入式文件数据库（原子替换落盘、事务写入可回滚），四表 `config` / `sensor_raw` / `processed_data` / `system_events`；差分 + 变长编码归档压缩、四分区存储、LRU 数据老化；时间 / Geohash 空间 / 特征直方图三维索引。
- **通信与协同**：链路质量评分与智能切换（信号 30% + 成功率 40% + 延迟 20% + 能耗 10%）；自适应协议 WiFi → UDP/TLV、蜂窝 → MQTT over TLS、LPWAN → CoAP/CBOR；三级 QoS、AIMD 滑动窗口拥塞控制、令牌桶限流、CRC32 智能分片；TLS 1.3 双向证书、应用层密钥 24 小时轮换；原子配置事务与差分 OTA。融合数据经 MQTT 上传，收到云端确认后更新同步状态。
- **系统自治理**：四维资源监控三级告警、5 分钟滑动窗口性能分析；归一化马氏距离异常评分（>2 可能 / >3 很可能 / >5 严重）分级自动恢复；四级调度（优先级老化 + 资源预留）、软件看门狗心跳；五级功耗管理、电池健康与维护预警。

## 03 · 构建与运行

| 平台 | 工具链 |
|:--|:--|
| Windows | MinGW-w64（GCC 9+）或 MSVC 2019+ |
| Linux | GCC 9+ / Clang 10+ |

CMake（先配置，再构建）：
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
```
```bash
cmake --build build -j
```

直接编译（Linux）：
```bash
g++ -std=c++17 -O2 -Iinclude src/*.cpp -o soil_collection_system
```

直接编译（Windows / MinGW）：
```bash
g++ -std=c++17 -O2 -Iinclude -static src/*.cpp -lws2_32 -o scs.exe
```

标准监测场景（默认运行 60 秒，面板 http://localhost:8080）：
```bash
./soil_collection_system
```

## 04 · 命令行参数

| 参数 | 说明 |
|:--|:--|
| `--duration <秒>` | 运行时长（默认 60，生产环境为常驻运行） |
| `--moving` | 仿真载具移动，启用距离触发采集模式 |
| `--fault` | 注入电导率传感器故障，演示自恢复流程 |
| `--quiet` | 控制台静默（日志仍实时推送至 Web 终端） |
| `--web <端口>` | Web 面板端口（默认 8080） |
| `--no-web` | 禁用 Web 面板 |

运行产物：`data/soil_database.json`（数据库）、`data/archive/`（归档块）、`logs/system.log`（日志）。

## 05 · Web 监控面板

| 路由 | 说明 |
|:--|:--|
| `GET /` | 监控面板页面 |
| `GET /api/status` | 系统状态快照（模块统计 / 传感器状态 / 最新数据点） |
| `GET /api/data?count=N` | 近期融合数据时序（图表数据源） |
| `GET /api/logs?count=N` | 最近日志条目 |
| `GET /api/stream` | SSE 实时日志流（内嵌终端数据源） |

面板提供内嵌终端（SSE 推送全系统日志，支持暂停 / 清空 / 自动滚动）、运行统计、传感器状态、数据质量分布与四路参数趋势曲线，亮 / 暗主题可切换；页面不依赖前端框架。

## 06 · 配置

传感器参数在 `config/sensors.json` 中配置（型号、总线地址、校准系数、采样间隔、优先级），支持远程配置事务热更新：
```json
{"model": "Capacitive-Soil-Moisture", "address": 19, "sample_rate_hz": 200,
 "calibration_coeffs": [0.0, 1.0, 0.0], "interval_ms": 5000, "priority": 1}
```

## 07 · 目标机移植

| 仿真接口 | 目标机实现 |
|:--|:--|
| `i2c_read/write` | Linux i2c-dev（ioctl I2C_RDWR） |
| `spi_transfer` | spidev |
| `uart_send/receive` | termios（SDI-12 / RS-485 / Modbus 帧收发） |
| `adc_read` | 处理器内置 ADC 或外置 ADS1256 |
| `get_gps_data` | UART + NMEA-0183 解析 + PPS 秒脉冲 |
| `receive_can_signal` | SocketCAN |
| 文件数据库 | 可等价替换为 SQLite3 |
| MQTT / TLS | 可替换为 Eclipse Paho / mbedTLS |

## 08 · 路线图（未实现）

- 原生硬件抽象层实现（i2c-dev / spidev / SocketCAN）
- SQLite3 持久化引擎
- mbedTLS 真实 AES-GCM 与证书管理
- OTA 差分包签名校验
- 单元测试与 CI
- 英文文档

---

<div align="right"><sub>从像素到混凝土 · <a href="https://github.com/iCxin">iCxin</a></sub></div>
