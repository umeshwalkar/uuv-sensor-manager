#pragma once
#include <string>
#include <vector>
#include <stdexcept>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// ============================================================================
// Transport Configuration (UDP Server, TCP Client, TCP Server, Serial RS232)
// ============================================================================

struct TransportConfig {
    std::string type               = "tcp_client"; // udp_server | tcp_client | tcp_server | serial
    std::string bind_host          = "0.0.0.0";
    int         bind_port          = 0;
    std::string host;                              // tcp_client target
    int         port               = 0;
    std::string serial_port        = "";
    int         serial_baud        = 9600;
    int         serial_data_bits   = 8;
    int         serial_stop_bits   = 1;
    std::string serial_parity      = "N";          // N=None, E=Even, O=Odd
    int         connect_timeout_sec  = 5;
    int         reconnect_delay_sec  = 3;
    int         read_timeout_ms      = 1000;
    int         buffer_size_bytes    = 512;
};

// ============================================================================
// MQTT Configuration
// ============================================================================

struct MqttTopics {
    std::string sensor    = "uuv/Sensor";
    std::string status = "Sensor/status";
};

struct MqttConfig {
    std::string broker    = "localhost";
    int         port      = 1883;
    std::string client_id = "Sensor_manager";
    int         keepalive = 60;
    int         qos       = 1;
    bool        retain    = false;
    bool        enabled   = true;
    MqttTopics  topics;
};

// ============================================================================
// Sensor Device Connection Configuration
// ============================================================================

struct SensorDeviceConfig {
    std::string     name             = "Sensor_device";
    bool            enabled          = true;
    TransportConfig transport;
    double          data_timeout_sec = 2.0;
};

// ============================================================================
// Sensor Application Configuration
// ============================================================================

struct SensorConfig {
    int          publish_interval_ms    = 1000;
    double       data_timeout_sec       = 2.0;
    int          status_interval_sec    = 10;
    bool         publish_raw_data       = false;
    bool         send_init_on_reconnect = false;  // re-send init_commands after every reconnect

    std::vector<std::string> init_commands;        // sent to device at startup / on (re)connect

    SensorDeviceConfig sensor_device;
};

// ============================================================================
// Debug / Logging Configuration
// ============================================================================

struct DebugConfig {
    bool        enabled = true;
    std::string level   = "info";   // "debug" | "info" | "warning" | "error"
};

// ============================================================================
// Application Configuration (top-level)
// ============================================================================

struct AppConfig {
    MqttConfig  mqtt;
    SensorConfig   sensor;
    DebugConfig debug;

    static AppConfig fromFile(const std::string& path);     // auto-detect .json / .ini
    static AppConfig fromJsonFile(const std::string& path);
    static AppConfig fromIniFile(const std::string& path);
};
