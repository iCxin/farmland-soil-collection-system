// =============================================================================
// main.cpp - 农田土壤参数移动采集系统 V1.0 入口
//
// 用法:
//   soil_collection_system [选项]
// 选项:
//   --duration <秒>    运行时长（默认 60 秒）
//   --moving           仿真载具移动（启用距离触发采集模式）
//   --fault            注入电导率传感器故障（演示异常检测与自恢复）
//   --quiet            精简控制台输出
//   --help             显示帮助
// =============================================================================
#include "system_orchestrator.h"

#include <cstring>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <csignal>

namespace {
soil::SystemOrchestrator* g_orchestrator = nullptr;

void signalHandler(int sig) {
    (void)sig;
    // 收到终止信号（Ctrl+C）：请求主循环退出，由 run() 统一执行优雅停机
    if (g_orchestrator) {
        g_orchestrator->requestStop();
    }
}
}  // namespace

int main(int argc, char* argv[]) {
#if defined(_WIN32)
    // Windows 控制台 UTF-8 输出
    SetConsoleOutputCP(CP_UTF8);
#endif

    soil::SystemRunOptions options;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--duration") == 0 && i + 1 < argc) {
            options.duration_seconds = std::atoi(argv[++i]);
            if (options.duration_seconds <= 0) options.duration_seconds = 60;
        } else if (std::strcmp(argv[i], "--moving") == 0) {
            options.vehicle_moving = true;
        } else if (std::strcmp(argv[i], "--fault") == 0) {
            options.inject_sensor_fault = true;
        } else if (std::strcmp(argv[i], "--quiet") == 0) {
            options.quiet_console = true;
        } else if (std::strcmp(argv[i], "--web") == 0 && i + 1 < argc) {
            options.web_port = std::atoi(argv[++i]);
            if (options.web_port <= 0 || options.web_port > 65535) options.web_port = 8080;
        } else if (std::strcmp(argv[i], "--no-web") == 0) {
            options.web_enabled = false;
        } else if (std::strcmp(argv[i], "--help") == 0) {
            std::cout << "农田土壤参数移动采集系统 V1.0\n"
                      << "用法: soil_collection_system [选项]\n"
                      << "  --duration <秒>    运行时长（默认 60）\n"
                      << "  --moving           仿真载具移动（距离触发采集）\n"
                      << "  --fault            注入电导率传感器故障（演示自恢复）\n"
                      << "  --quiet            精简控制台输出（Web终端仍实时刷新）\n"
                      << "  --web <端口>       Web 面板端口（默认 8080）\n"
                      << "  --no-web           禁用 Web 监控面板\n";
            return 0;
        }
    }

    soil::SystemOrchestrator orchestrator(options);
    g_orchestrator = &orchestrator;
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    if (!orchestrator.startup()) {
        std::cerr << "系统启动失败" << std::endl;
        return 1;
    }
    orchestrator.run();
    return 0;
}
