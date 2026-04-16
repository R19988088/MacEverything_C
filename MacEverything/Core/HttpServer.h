#pragma once
#include <string>
#include <memory>
#include <atomic>
#include <thread>
#include <cstdint>
#include <unordered_map>

class SearchEngine;
class ContentIndex;

class HttpServer {
public:
    HttpServer() = default;
    ~HttpServer();
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    void start(uint16_t port,
               std::shared_ptr<SearchEngine> engine,
               std::shared_ptr<ContentIndex> contentIndex);
    void stop();
    bool isRunning() const;
    uint16_t port() const;

private:
    void acceptLoop();
    void handleConnection(int clientFd);

    struct HttpRequest {
        std::string method;
        std::string path;
        std::unordered_map<std::string, std::string> query;
    };

    HttpRequest parseRequest(const std::string& raw);
    std::string route(const HttpRequest& req);

    std::string handleSearch(const std::unordered_map<std::string, std::string>& params);
    std::string handleContentSearch(const std::unordered_map<std::string, std::string>& params);
    std::string handleRecent(const std::unordered_map<std::string, std::string>& params);
    std::string handleStatus();
    std::string handleHealth();

    std::string jsonResponse(int status, const std::string& body);
    std::string errorResponse(int status, const std::string& message);

    std::shared_ptr<SearchEngine> engine_;
    std::shared_ptr<ContentIndex> contentIndex_;
    std::atomic<bool> running_{false};
    int serverFd_{-1};
    uint16_t port_{0};
    std::thread acceptThread_;
};
