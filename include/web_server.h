// =============================================================================
// web_server.h - 嵌入式 Web 监控服务器
//
// 轻量级 HTTP 服务（零第三方依赖），为浏览器提供：
//   GET  /              实时监控面板（内嵌网页：仪表盘 + 终端 + 图表）
//   GET  /api/status    系统状态 JSON（2秒轮询）
//   GET  /api/data      近期融合数据 JSON（趋势图表）
//   GET  /api/logs      最近日志 JSON（终端回填）
//   GET  /api/stream    SSE 实时日志流（内嵌终端）
//   GET  /api/reboot    (POST 语义保留) 远程重启演示
// =============================================================================
#pragma once

#include "common.h"
#include "logger.h"

#include <atomic>
#include <condition_variable>
#include <deque>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace soil {

#if defined(_WIN32)
typedef SOCKET socket_t;
#else
typedef int socket_t;
#endif

class SystemOrchestrator;
class StorageModule;

class WebServer {
public:
    WebServer(SystemOrchestrator& orchestrator, StorageModule& storage, uint16_t port);
    ~WebServer();

    bool start();
    void stop();

    // 日志广播入口（由 Logger 广播钩子调用，推送至所有 SSE 终端客户端）
    void broadcastLog(const DiagnosticLogEntry& entry);

    uint16_t port() const { return port_; }

private:
    // SSE 订阅者
    struct Subscriber {
        std::mutex mutex;
        std::condition_variable cv;
        std::deque<std::string> pending;
        bool closed = false;
    };

    void acceptLoop();
    void handleConnection(socket_t fd);
    void handleSse(socket_t fd);
    void handleStatus(std::string& body);
    void handleData(int count, std::string& body);
    void handleLogs(int count, std::string& body);

    static bool sendAll(socket_t fd, const char* data, size_t len);
    static void sendResponse(socket_t fd, const char* content_type,
                             const std::string& body);
    static std::string jsonEscape(const std::string& s);

    SystemOrchestrator& orchestrator_;
    StorageModule& storage_;
    uint16_t port_;
    socket_t listen_fd_ = -1;
    std::atomic<bool> running_{false};
    std::thread accept_thread_;

    std::mutex subs_mutex_;
    std::vector<std::shared_ptr<Subscriber>> subscribers_;
};

}  // namespace soil
