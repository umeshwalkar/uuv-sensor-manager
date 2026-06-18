#pragma once
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include "Config.hpp"
#include "Transport.hpp"
#include "MqttClient.hpp"
#include "SensorParser.hpp"

/**
 * Point-in-time snapshot of the latest SVP reading (thread-safe copy)
 */
struct SensorSnapshot {
    SensorData data;
    std::chrono::system_clock::time_point timestamp;
    bool     is_valid         = false;
    uint64_t packets_received = 0;
    uint64_t parse_errors     = 0;
};

/**
 * SensorManager — connects to a Sensor device over TCP, UDP, or serial;
 * sends init commands on first connection (and optionally on every reconnect);
 * continuously receives and parses data frames; publishes parsed data as JSON
 * to uuv/sensor and statistics to sensor/status at configurable intervals.
 */
class SensorManager {
public:
    explicit SensorManager(const AppConfig& cfg);
    ~SensorManager();

    void run();     // blocks until stop() is called
    void stop();    // safe to call from signal handler

private:
    AppConfig                   cfg_;
    std::unique_ptr<ITransport> transport_;
    std::unique_ptr<MqttClient> mqtt_;

    std::atomic<bool>           running_{false};
    std::thread                 receive_thread_;
    std::thread                 publish_thread_;

    std::condition_variable     cv_;
    mutable std::mutex          cv_mutex_;
    mutable std::mutex          snapshot_mutex_;
    SensorSnapshot              snapshot_;

    uint64_t                    packets_received_ = 0;
    uint64_t                    parse_errors_     = 0;

    void receiveLoop();
    void publishLoop();

    void        sendInitCommands();
    std::string formatDataJson(const SensorSnapshot& snap) const;
    std::string formatStatusJson() const;
};
