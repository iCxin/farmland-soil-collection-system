// =============================================================================
// web_server.cpp - 嵌入式 Web 监控服务器实现
// =============================================================================
#include "web_server.h"
#include "web_assets.h"

#include "autonomy_module.h"
#include "comm_module.h"
#include "fusion_module.h"
#include "sensing_module.h"
#include "storage_module.h"
#include "system_orchestrator.h"

#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#define CLOSE_SOCKET closesocket
#else
#define CLOSE_SOCKET ::close
#define SOCKET_ERROR (-1)
#define INVALID_SOCKET (-1)
#endif

namespace soil {

namespace {
const char* contentTypeOf(const std::string& path) {
    (void)path;
    return "text/html; charset=utf-8";
}
}  // namespace

WebServer::WebServer(SystemOrchestrator& orchestrator, StorageModule& storage,
                     uint16_t port)
    : orchestrator_(orchestrator), storage_(storage), port_(port) {}

WebServer::~WebServer() { stop(); }

bool WebServer::start() {
#if defined(_WIN32)
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
#else
    signal(SIGPIPE, SIG_IGN);  // SSE 客户端断开时不产生 SIGPIPE
#endif

    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ == INVALID_SOCKET) return false;

    int reuse = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port_);
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) ==
        SOCKET_ERROR) {
        CLOSE_SOCKET(listen_fd_);
        listen_fd_ = INVALID_SOCKET;
        return false;
    }
    if (::listen(listen_fd_, 8) == SOCKET_ERROR) {
        CLOSE_SOCKET(listen_fd_);
        listen_fd_ = INVALID_SOCKET;
        return false;
    }

    running_ = true;
    accept_thread_ = std::thread(&WebServer::acceptLoop, this);

    // 注册日志广播钩子（SSE 终端流数据源）
    Logger::instance().setBroadcastHook(
        [this](const DiagnosticLogEntry& e) { broadcastLog(e); });
    return true;
}

void WebServer::stop() {
    if (!running_.exchange(false)) return;
    Logger::instance().setBroadcastHook(nullptr);
    if (listen_fd_ != INVALID_SOCKET) {
        CLOSE_SOCKET(listen_fd_);
        listen_fd_ = INVALID_SOCKET;
    }
    // 唤醒所有 SSE 订阅线程
    {
        std::lock_guard<std::mutex> lock(subs_mutex_);
        for (auto& sub : subscribers_) {
            {
                std::lock_guard<std::mutex> sl(sub->mutex);
                sub->closed = true;
            }
            sub->cv.notify_all();
        }
    }
    if (accept_thread_.joinable()) accept_thread_.join();
#if defined(_WIN32)
    WSACleanup();
#endif
}

// -----------------------------------------------------------------------------
// 日志广播：推送到所有 SSE 订阅者
// -----------------------------------------------------------------------------
void WebServer::broadcastLog(const DiagnosticLogEntry& entry) {
    // 格式化为终端行
    std::ostringstream oss;
    oss << '[' << formatTimestamp(entry.timestamp) << "]["
        << (entry.level == LogLevel::Info
                ? "INFO "
                : entry.level == LogLevel::Warning ? "WARN " : "ERROR")
        << "][" << entry.module << "] " << entry.message;

    std::string json = "{\"t\":\"" + formatTimestamp(entry.timestamp) +
                       "\",\"lv\":\"" +
                       (entry.level == LogLevel::Info
                            ? "INFO"
                            : entry.level == LogLevel::Warning ? "WARN" : "ERROR") +
                       "\",\"l\":" +
                       std::to_string(static_cast<int>(entry.level)) +
                       ",\"m\":\"" + jsonEscape(entry.module) + "\",\"msg\":\"" +
                       jsonEscape(entry.message) + "\"}";

    std::lock_guard<std::mutex> lock(subs_mutex_);
    for (auto& sub : subscribers_) {
        std::lock_guard<std::mutex> sl(sub->mutex);
        if (sub->pending.size() < 256) {
            sub->pending.push_back("data: " + json + "\n\n");
        }
        sub->cv.notify_all();
    }
    (void)oss;
}

// -----------------------------------------------------------------------------
// 连接接受循环
// -----------------------------------------------------------------------------
void WebServer::acceptLoop() {
    while (running_) {
        sockaddr_in peer{};
#if defined(_WIN32)
        int peer_len = static_cast<int>(sizeof(peer));
#else
        socklen_t peer_len = sizeof(peer);
#endif
        socket_t fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&peer),
                               &peer_len);
        if (fd == INVALID_SOCKET) {
            if (running_) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            break;
        }
        // 每连接独立线程（监控场景并发量低，线程创建开销可接受）
        std::thread(&WebServer::handleConnection, this, fd).detach();
    }
}

bool WebServer::sendAll(socket_t fd, const char* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
#if defined(_WIN32)
        int n = ::send(fd, data + sent, static_cast<int>(len - sent), 0);
#else
        ssize_t n = ::send(fd, data + sent, len - sent, MSG_NOSIGNAL);
#endif
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

void WebServer::sendResponse(socket_t fd, const char* content_type,
                             const std::string& body) {
    std::ostringstream oss;
    oss << "HTTP/1.1 200 OK\r\n"
        << "Content-Type: " << content_type << "\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Access-Control-Allow-Origin: *\r\n"
        << "Connection: close\r\n"
        << "\r\n"
        << body;
    std::string resp = oss.str();
    sendAll(fd, resp.data(), resp.size());
}

// -----------------------------------------------------------------------------
// 请求处理与路由
// -----------------------------------------------------------------------------
void WebServer::handleConnection(socket_t fd) {
    char buf[4096] = {0};
    size_t got = 0;
    // 读取请求头（至多 4KB，本服务不处理请求体）
    while (got < sizeof(buf) - 1) {
#if defined(_WIN32)
        int n = ::recv(fd, buf + static_cast<int>(got),
                       static_cast<int>(sizeof(buf) - 1 - got), 0);
#else
        ssize_t n = ::recv(fd, buf + got, sizeof(buf) - 1 - got, 0);
#endif
        if (n <= 0) break;
        got += static_cast<size_t>(n);
        if (std::strstr(buf, "\r\n\r\n")) break;
    }
    buf[got] = '\0';

    // 解析请求行：GET /path HTTP/1.1
    std::string request(buf);
    std::string method = "GET", path = "/", query;
    {
        size_t sp1 = request.find(' ');
        size_t sp2 = request.find(' ', sp1 + 1);
        if (sp1 != std::string::npos && sp2 != std::string::npos) {
            method = request.substr(0, sp1);
            path = request.substr(sp1 + 1, sp2 - sp1 - 1);
        }
        size_t q = path.find('?');
        if (q != std::string::npos) {
            query = path.substr(q + 1);
            path = path.substr(0, q);
        }
    }

    auto queryParam = [&](const std::string& key, int fallback) -> int {
        size_t k = query.find(key + "=");
        if (k == std::string::npos) return fallback;
        return std::atoi(query.c_str() + k + key.size() + 1);
    };

    if (path == "/" || path == "/index.html") {
        sendResponse(fd, contentTypeOf(path), DASHBOARD_HTML);
    } else if (path == "/favicon.ico") {
        // 内嵌 SVG 站点图标（绿色幼苗）
        std::string icon =
            "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 16 16'>"
            "<text y='13' font-size='13'>🌱</text></svg>";
        sendResponse(fd, "image/svg+xml", icon);
    } else if (path == "/api/status") {
        std::string body;
        handleStatus(body);
        sendResponse(fd, "application/json; charset=utf-8", body);
    } else if (path == "/api/data") {
        std::string body;
        handleData(queryParam("count", 60), body);
        sendResponse(fd, "application/json; charset=utf-8", body);
    } else if (path == "/api/logs") {
        std::string body;
        handleLogs(queryParam("count", 100), body);
        sendResponse(fd, "application/json; charset=utf-8", body);
    } else if (path == "/api/stream" && method == "GET") {
        handleSse(fd);
    } else {
        std::string body = "{\"error\":\"not found\"}";
        std::ostringstream oss;
        oss << "HTTP/1.1 404 Not Found\r\n"
            << "Content-Type: application/json; charset=utf-8\r\n"
            << "Content-Length: " << body.size() << "\r\n"
            << "Connection: close\r\n\r\n"
            << body;
        std::string resp = oss.str();
        sendAll(fd, resp.data(), resp.size());
    }
    CLOSE_SOCKET(fd);
}

// -----------------------------------------------------------------------------
// SSE 实时日志流（内嵌终端数据源）
// -----------------------------------------------------------------------------
void WebServer::handleSse(socket_t fd) {
    auto sub = std::make_shared<Subscriber>();
    {
        std::lock_guard<std::mutex> lock(subs_mutex_);
        subscribers_.push_back(sub);
    }

    // HTTP 响应头（SSE 流）
    std::string headers =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream; charset=utf-8\r\n"
        "Cache-Control: no-cache\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: keep-alive\r\n\r\n"
        "retry: 3000\n\n";
    if (!sendAll(fd, headers.data(), headers.size())) {
        std::lock_guard<std::mutex> lock(subs_mutex_);
        sub->closed = true;
        return;
    }

    auto last_beat = std::chrono::steady_clock::now();
    while (running_ && !sub->closed) {
        std::string chunk;
        {
            std::unique_lock<std::mutex> lock(sub->mutex);
            if (sub->pending.empty()) {
                // 每15秒发送心跳注释，保持连接活性
                if (std::chrono::steady_clock::now() - last_beat <
                    std::chrono::seconds(15)) {
                    sub->cv.wait_for(lock, std::chrono::seconds(1));
                    continue;
                }
                last_beat = std::chrono::steady_clock::now();
                chunk = ": heartbeat\n\n";
            } else {
                chunk = std::move(sub->pending.front());
                sub->pending.pop_front();
            }
        }
        if (!sendAll(fd, chunk.data(), chunk.size())) {
            sub->closed = true;
            break;
        }
    }

    // 移除订阅者
    {
        std::lock_guard<std::mutex> lock(subs_mutex_);
        std::lock_guard<std::mutex> sl(sub->mutex);
        sub->closed = true;
        subscribers_.erase(
            std::remove_if(subscribers_.begin(), subscribers_.end(),
                           [sub](const std::shared_ptr<Subscriber>& s) {
                               return s.get() == sub.get();
                           }),
            subscribers_.end());
    }
}

// -----------------------------------------------------------------------------
// GET /api/status — 系统状态快照
// -----------------------------------------------------------------------------
void WebServer::handleStatus(std::string& body) {
    auto st = orchestrator_.statusSnapshot();
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2);
    oss << "{\"device_id\":\"" << jsonEscape(st.device_id) << "\""
        << ",\"version\":\"" << jsonEscape(st.version) << "\""
        << ",\"uptime_s\":" << st.uptime_s
        << ",\"health\":" << st.health
        << ",\"device_state\":\"" << jsonEscape(st.device_state) << "\""
        << ",\"network\":{\"online\":" << (st.network_online ? "true" : "false")
        << ",\"interface\":\"" << jsonEscape(st.network_interface) << "\"}"
        << ",\"sensors\":[";
    for (size_t i = 0; i < st.sensors.size(); ++i) {
        oss << "{\"name\":\"" << jsonEscape(st.sensors[i].name)
            << "\",\"code\":\"" << jsonEscape(st.sensors[i].code)
            << "\",\"status\":\"" << jsonEscape(st.sensors[i].status) << "\"}"
            << (i + 1 < st.sensors.size() ? "," : "");
    }
    oss << "],\"sensing\":{\"samples\":" << st.sensing_samples
        << ",\"outliers\":" << st.sensing_outliers
        << ",\"errors\":" << st.sensing_errors
        << ",\"distance_triggered\":" << st.sensing_distance
        << ",\"high_freq\":" << st.sensing_high_freq << "}"
        << ",\"fusion\":{\"points\":" << st.fusion_points << "}"
        << ",\"storage\":{\"commits\":" << st.storage_commits
        << ",\"unsynced\":" << st.storage_unsynced
        << ",\"archived\":" << st.storage_archived
        << ",\"processed\":" << st.storage_processed << "}"
        << ",\"comm\":{\"uploaded\":" << st.comm_uploaded
        << ",\"fragments\":" << st.comm_fragments
        << ",\"ack_fail\":" << st.comm_ack_fail << "}"
        << ",\"autonomy\":{\"anomalies\":" << st.autonomy_anomalies
        << ",\"recoveries\":" << st.autonomy_recoveries << "}"
        << ",\"quality\":{\"excellent\":" << st.q_excellent
        << ",\"good\":" << st.q_good << ",\"fair\":" << st.q_fair
        << ",\"poor\":" << st.q_poor << "}";
    if (st.has_latest) {
        oss << ",\"latest\":{\"batch\":" << st.latest_batch
            << ",\"temp\":" << st.latest_temp << ",\"moisture\":" << st.latest_moisture
            << ",\"ph\":" << st.latest_ph << ",\"ec\":" << st.latest_ec
            << ",\"quality\":" << st.latest_quality << "}";
    }
    oss << "}";
    body = oss.str();
}

// -----------------------------------------------------------------------------
// GET /api/data?count=N — 近期融合数据（趋势图表）
// -----------------------------------------------------------------------------
void WebServer::handleData(int count, std::string& body) {
    count = std::max(10, std::min(count, 300));
    auto now_ms = toUnixMs(std::chrono::system_clock::now());
    auto rows = storage_.queryByTimeRange(now_ms - 3600 * 1000LL, now_ms, count);
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(3);
    oss << "[";
    for (size_t i = 0; i < rows.size(); ++i) {
        const auto& r = rows[i];
        oss << "{\"ts\":" << r.timestamp_ms << ",\"temp\":" << r.soil_temp
            << ",\"moisture\":" << r.soil_moisture << ",\"ph\":" << r.soil_ph
            << ",\"ec\":" << r.soil_ec << "}"
            << (i + 1 < rows.size() ? "," : "");
    }
    oss << "]";
    body = oss.str();
}

// -----------------------------------------------------------------------------
// GET /api/logs?count=N — 最近日志（终端回填）
// -----------------------------------------------------------------------------
void WebServer::handleLogs(int count, std::string& body) {
    count = std::max(1, std::min(count, 500));
    auto entries = Logger::instance().recentEntries(static_cast<size_t>(count));
    std::ostringstream oss;
    oss << "[";
    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& e = entries[i];
        oss << "{\"t\":\"" << formatTimestamp(e.timestamp) << "\",\"lv\":\""
            << (e.level == LogLevel::Info
                    ? "INFO"
                    : e.level == LogLevel::Warning ? "WARN" : "ERROR")
            << "\",\"l\":" << static_cast<int>(e.level)
            << ",\"m\":\"" << jsonEscape(e.module) << "\",\"msg\":\""
            << jsonEscape(e.message) << "\"}"
            << (i + 1 < entries.size() ? "," : "");
    }
    oss << "]";
    body = oss.str();
}

std::string WebServer::jsonEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c;
        }
    }
    return out;
}

}  // namespace soil
