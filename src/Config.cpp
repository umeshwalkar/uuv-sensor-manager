#include "Config.hpp"
#include <fstream>
#include <algorithm>
#include <map>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// ── helpers ───────────────────────────────────────────────────────────────────

static TransportConfig parseTransport(const json& t) {
    TransportConfig cfg;
    if (t.contains("type"))                cfg.type                = t.value("type",                cfg.type);
    if (t.contains("bind_host"))           cfg.bind_host           = t.value("bind_host",           cfg.bind_host);
    if (t.contains("bind_port"))           cfg.bind_port           = t.value("bind_port",           cfg.bind_port);
    if (t.contains("host"))                cfg.host                = t.value("host",                cfg.host);
    if (t.contains("port"))                cfg.port                = t.value("port",                cfg.port);
    if (t.contains("serial_port"))         cfg.serial_port         = t.value("serial_port",         cfg.serial_port);
    if (t.contains("serial_baud"))         cfg.serial_baud         = t.value("serial_baud",         cfg.serial_baud);
    if (t.contains("serial_data_bits"))    cfg.serial_data_bits    = t.value("serial_data_bits",    cfg.serial_data_bits);
    if (t.contains("serial_stop_bits"))    cfg.serial_stop_bits    = t.value("serial_stop_bits",    cfg.serial_stop_bits);
    if (t.contains("serial_parity"))       cfg.serial_parity       = t.value("serial_parity",       cfg.serial_parity);
    if (t.contains("connect_timeout_sec")) cfg.connect_timeout_sec = t.value("connect_timeout_sec", cfg.connect_timeout_sec);
    if (t.contains("reconnect_delay_sec")) cfg.reconnect_delay_sec = t.value("reconnect_delay_sec", cfg.reconnect_delay_sec);
    if (t.contains("read_timeout_ms"))     cfg.read_timeout_ms     = t.value("read_timeout_ms",     cfg.read_timeout_ms);
    if (t.contains("buffer_size_bytes"))   cfg.buffer_size_bytes   = t.value("buffer_size_bytes",   cfg.buffer_size_bytes);
    return cfg;
}

// ── auto-detect ───────────────────────────────────────────────────────────────

AppConfig AppConfig::fromFile(const std::string& path) {
    std::string ext;
    size_t pos = path.rfind('.');
    if (pos != std::string::npos) {
        ext = path.substr(pos);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    }
    if (ext == ".json") return fromJsonFile(path);
    if (ext == ".ini")  return fromIniFile(path);
    throw std::runtime_error("Unknown config format: " + path + " (expected .json or .ini)");
}

// ── JSON loader ───────────────────────────────────────────────────────────────

AppConfig AppConfig::fromJsonFile(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open())
        throw std::runtime_error("Failed to open config file: " + path);

    json config;
    try { file >> config; }
    catch (const std::exception& e) {
        throw std::runtime_error("Failed to parse JSON config: " + std::string(e.what()));
    }

    AppConfig cfg;

    // ── MQTT ─────────────────────────────────────────────────────────────────
    if (config.contains("mqtt")) {
        auto& m = config["mqtt"];
        cfg.mqtt.enabled   = m.value("enabled",   true);
        cfg.mqtt.broker    = m.value("broker",    "localhost");
        cfg.mqtt.port      = m.value("port",      1883);
        cfg.mqtt.client_id = m.value("client_id", "sensor_manager");
        cfg.mqtt.keepalive = m.value("keepalive", 60);
        cfg.mqtt.qos       = m.value("qos",       1);
        cfg.mqtt.retain    = m.value("retain",    false);

        if (m.contains("topics")) {
            auto& topics = m["topics"];
            if (topics.contains("pub") && topics["pub"].is_array()) {
                // New format: topics.pub[] array of {name, topic, ...}
                for (auto& t : topics["pub"]) {
                    std::string name  = t.value("name",  "");
                    std::string topic = t.value("topic", "");
                    if      (name == "sensor")    cfg.mqtt.topics.sensor    = topic;
                    else if (name == "status") cfg.mqtt.topics.status = topic;
                }
            } 
        }
    }

    // ── SVP ──────────────────────────────────────────────────────────────────
    if (config.contains("sensor")) {
        auto& c = config["sensor"];

        // Legacy flat fields — read as defaults; overridden below by device format.
        cfg.sensor.publish_interval_ms    = c.value("publish_interval_ms",    1000);
        cfg.sensor.data_timeout_sec       = c.value("data_timeout_sec",       2.0);
        cfg.sensor.status_interval_sec    = c.value("status_interval_sec",    10);
        cfg.sensor.publish_raw_data       = c.value("publish_raw_data",       false);
        cfg.sensor.send_init_on_reconnect = c.value("send_init_on_reconnect", false);

        if (c.contains("init_commands") && c["init_commands"].is_array()) {
            for (auto& cmd : c["init_commands"])
                cfg.sensor.init_commands.push_back(cmd.get<std::string>());
        }

        // New format: sensor.transport[] — build id -> TransportConfig map
        std::map<std::string, TransportConfig> transport_map;
        if (c.contains("transport") && c["transport"].is_array()) {
            for (auto& t : c["transport"]) {
                if (!t.contains("id")) continue;
                transport_map[t["id"].get<std::string>()] = parseTransport(t);
            }
        }

        // New format: sensor.devices[] — use the first device entry
        if (c.contains("devices") && c["devices"].is_array() && !c["devices"].empty()) {
            auto& dev = c["devices"][0];

            cfg.sensor.sensor_device.name        = dev.value("name",                   "sensor_device");
            cfg.sensor.sensor_device.enabled     = dev.value("publish_enabled",        true);
            cfg.sensor.publish_interval_ms    = dev.value("publish_interval_ms",    cfg.sensor.publish_interval_ms);
            cfg.sensor.publish_raw_data       = dev.value("publish_raw_data",       cfg.sensor.publish_raw_data);
            cfg.sensor.send_init_on_reconnect = dev.value("send_init_on_reconnect", cfg.sensor.send_init_on_reconnect);

            if (dev.contains("init_commands") && dev["init_commands"].is_array()) {
                cfg.sensor.init_commands.clear();
                for (auto& cmd : dev["init_commands"])
                    cfg.sensor.init_commands.push_back(cmd.get<std::string>());
            }

            // Resolve transport via input_channels.data_rx.transport.id
            if (dev.contains("input_channels") &&
                dev["input_channels"].contains("data_rx")) {
                auto& rx = dev["input_channels"]["data_rx"];
                cfg.sensor.data_timeout_sec            = rx.value("data_timeout_sec", cfg.sensor.data_timeout_sec);
                cfg.sensor.sensor_device.data_timeout_sec = cfg.sensor.data_timeout_sec;

                if (rx.contains("transport")) {
                    auto& rt      = rx["transport"];
                    std::string id = rt.value("id", "");
                    auto it = transport_map.find(id);
                    if (it != transport_map.end()) {
                        cfg.sensor.sensor_device.transport = it->second;
                        // Channel-level read_timeout_ms overrides the transport default.
                        cfg.sensor.sensor_device.transport.read_timeout_ms =
                            rt.value("read_timeout_ms", cfg.sensor.sensor_device.transport.read_timeout_ms);
                    }
                }
            }
        }

 
    }

    // ── Debug ─────────────────────────────────────────────────────────────────
    if (config.contains("debug")) {
        auto& d = config["debug"];
        cfg.debug.enabled = d.value("enabled", true);
        cfg.debug.level   = d.value("level",   "info");
    }

    return cfg;
}

// ── INI loader ────────────────────────────────────────────────────────────────

AppConfig AppConfig::fromIniFile(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open())
        throw std::runtime_error("Failed to open config file: " + path);

    AppConfig cfg;
    std::string line, section;

    auto getB = [](const std::string& v) { return v == "true" || v == "1"; };
    auto getI = [](const std::string& v) { return std::stoi(v); };
    auto getD = [](const std::string& v) { return std::stod(v); };

    while (std::getline(file, line)) {
        // Strip UTF-8 BOM
        if (line.size() >= 3 &&
            (unsigned char)line[0] == 0xEF &&
            (unsigned char)line[1] == 0xBB &&
            (unsigned char)line[2] == 0xBF)
            line.erase(0, 3);

        // Trim
        line.erase(0, line.find_first_not_of(" \t\r\n"));
        auto last = line.find_last_not_of(" \t\r\n");
        if (last == std::string::npos) continue;
        line.erase(last + 1);

        if (line.empty() || line[0] == ';' || line[0] == '#') continue;

        if (line[0] == '[' && line.back() == ']') {
            section = line.substr(1, line.size() - 2);
            continue;
        }

        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        key.erase(key.find_last_not_of(" \t") + 1);
        val.erase(0, val.find_first_not_of(" \t"));
        if (val.empty()) continue;

        // [mqtt]
        if (section == "mqtt") {
            if (key == "enabled")    cfg.mqtt.enabled   = getB(val);
            if (key == "broker")     cfg.mqtt.broker    = val;
            if (key == "port")       cfg.mqtt.port      = getI(val);
            if (key == "client_id")  cfg.mqtt.client_id = val;
            if (key == "keepalive")  cfg.mqtt.keepalive = getI(val);
            if (key == "qos")        cfg.mqtt.qos       = getI(val);
            if (key == "retain")     cfg.mqtt.retain    = getB(val);
        }

        // [mqtt.topics]
        if (section == "mqtt.topics") {
            if (key == "sensor")    cfg.mqtt.topics.sensor    = val;
            if (key == "status") cfg.mqtt.topics.status = val;
        }

        // [sensor]
        if (section == "sensor") {
            if (key == "publish_interval_ms")    cfg.sensor.publish_interval_ms    = getI(val);
            if (key == "data_timeout_sec")       cfg.sensor.data_timeout_sec       = getD(val);
            if (key == "status_interval_sec")    cfg.sensor.status_interval_sec    = getI(val);
            if (key == "publish_raw_data")       cfg.sensor.publish_raw_data       = getB(val);
            if (key == "send_init_on_reconnect") cfg.sensor.send_init_on_reconnect = getB(val);
        }

        // [sensor.init_commands] — each value is one command, keys are ordering labels
        if (section == "sensor.init_commands") {
            cfg.sensor.init_commands.push_back(val);
        }

        // [debug]
        if (section == "debug") {
            if (key == "enabled") cfg.debug.enabled = getB(val);
            if (key == "level")   cfg.debug.level   = val;
        }

        // [sensor.device]
        if (section == "sensor.device") {
            if (key == "name")               cfg.sensor.sensor_device.name                         = val;
            if (key == "enabled")            cfg.sensor.sensor_device.enabled                      = getB(val);
            if (key == "data_timeout_sec")   cfg.sensor.sensor_device.data_timeout_sec             = getD(val);
            if (key == "type")               cfg.sensor.sensor_device.transport.type               = val;
            if (key == "bind_host")          cfg.sensor.sensor_device.transport.bind_host          = val;
            if (key == "bind_port")          cfg.sensor.sensor_device.transport.bind_port          = getI(val);
            if (key == "host")               cfg.sensor.sensor_device.transport.host               = val;
            if (key == "port")               cfg.sensor.sensor_device.transport.port               = getI(val);
            if (key == "serial_port")        cfg.sensor.sensor_device.transport.serial_port        = val;
            if (key == "serial_baud")        cfg.sensor.sensor_device.transport.serial_baud        = getI(val);
            if (key == "serial_data_bits")   cfg.sensor.sensor_device.transport.serial_data_bits   = getI(val);
            if (key == "serial_stop_bits")   cfg.sensor.sensor_device.transport.serial_stop_bits   = getI(val);
            if (key == "serial_parity")      cfg.sensor.sensor_device.transport.serial_parity      = val;
            if (key == "connect_timeout_sec") cfg.sensor.sensor_device.transport.connect_timeout_sec = getI(val);
            if (key == "reconnect_delay_sec") cfg.sensor.sensor_device.transport.reconnect_delay_sec = getI(val);
            if (key == "read_timeout_ms")    cfg.sensor.sensor_device.transport.read_timeout_ms    = getI(val);
            if (key == "buffer_size_bytes")  cfg.sensor.sensor_device.transport.buffer_size_bytes  = getI(val);
        }
    }

    return cfg;
}
