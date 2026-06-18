#include "SensorManager.hpp"
#include "Logger.hpp"
#include <sstream>
#include <iomanip>
#include <chrono>
#include <thread>

#define MOD "SensorManager"

// ── helpers ───────────────────────────────────────────────────────────────────

static double epochNow()
{
    return std::chrono::duration<double>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

static double epochOf(const std::chrono::system_clock::time_point &tp)
{
    return std::chrono::duration<double>(tp.time_since_epoch()).count();
}

// ── construction / destruction ────────────────────────────────────────────────

SensorManager::SensorManager(const AppConfig &cfg)
    : cfg_(cfg)
{
    transport_ = makeTransport(cfg_.sensor.sensor_device.transport);
    if (cfg_.mqtt.enabled)
        mqtt_ = std::make_unique<MqttClient>(cfg_.mqtt);
}

SensorManager::~SensorManager()
{
    stop();
}

// ── public interface ──────────────────────────────────────────────────────────

void SensorManager::run()
{
    if (running_)
        return;
    running_ = true;

    const auto &tr = cfg_.sensor.sensor_device.transport;
    LOG_INF(MOD, "Starting");
    LOG_INF(MOD, "  Transport    : %s", tr.type.c_str());
    if (tr.type == "tcp_client")
        LOG_INF(MOD, "  Device       : %s:%d", tr.host.c_str(), tr.port);
    else if (tr.type == "tcp_server" || tr.type == "udp_server")
        LOG_INF(MOD, "  Bind         : %s:%d", tr.bind_host.c_str(), tr.bind_port);
    else if (tr.type == "serial")
        LOG_INF(MOD, "  Port         : %s  baud=%d",
                tr.serial_port.c_str(), tr.serial_baud);
    LOG_INF(MOD, "  Init cmds    : %zu  resend_on_reconnect=%s",
            cfg_.sensor.init_commands.size(),
            cfg_.sensor.send_init_on_reconnect ? "yes" : "no");
    LOG_INF(MOD, "  Pub interval : %d ms  data_timeout=%.1fs",
            cfg_.sensor.publish_interval_ms, cfg_.sensor.data_timeout_sec);
    LOG_INF(MOD, "  SVP topic    : %s", cfg_.mqtt.topics.sensor.c_str());
    LOG_INF(MOD, "  Status topic : %s", cfg_.mqtt.topics.status.c_str());

    if (mqtt_)
    {
        if (!mqtt_->connect())
            LOG_WRN(MOD, "MQTT connect failed at startup — will retry on first publish");
    }
    else
    {
        LOG_WRN(MOD, "MQTT disabled — data will not be published");
    }

    receive_thread_ = std::thread(&SensorManager::receiveLoop, this);
    publish_thread_ = std::thread(&SensorManager::publishLoop, this);

    if (receive_thread_.joinable())
        receive_thread_.join();
    if (publish_thread_.joinable())
        publish_thread_.join();

    if (transport_->isOpen())
        transport_->close();

    LOG_INF(MOD, "Stopped — packets=%llu parse_errors=%llu",
            (unsigned long long)packets_received_,
            (unsigned long long)parse_errors_);
}

void SensorManager::stop()
{
    running_ = false;
    cv_.notify_all();
}

// ── init commands ─────────────────────────────────────────────────────────────

void SensorManager::sendInitCommands()
{
    if (cfg_.sensor.init_commands.empty())
        return;

    LOG_INF(MOD, "Sending %zu init command(s)", cfg_.sensor.init_commands.size());

    for (const auto &cmd : cfg_.sensor.init_commands)
    {
        std::string msg = cmd + "\r\n";
        LOG_DBG(MOD, "TX init cmd: %s", cmd.c_str());
        if (transport_->send(msg))
            LOG_DBG(MOD, "Init cmd sent OK: %s", cmd.c_str());
        else
            LOG_ERR(MOD, "Init cmd send FAILED: %s", cmd.c_str());
        // 150 ms gap between commands (interruptible)
        {
            std::unique_lock<std::mutex> lock(cv_mutex_);
            cv_.wait_for(lock, std::chrono::milliseconds(150),
                         [this]
                         { return !running_.load(); });
        }
        if (!running_)
            break;
    }
}

// ── receive loop ──────────────────────────────────────────────────────────────

void SensorManager::receiveLoop()
{
    LOG_INF("SensorManager::receiveLoop", "Started");

    const int reconnect_s = cfg_.sensor.sensor_device.transport.reconnect_delay_sec;
    const int timeout_ms = cfg_.sensor.sensor_device.transport.read_timeout_ms;
    const double data_timeout = cfg_.sensor.data_timeout_sec;
    bool init_sent = false;

    // Track last valid data for the "no data" warning.
    auto last_valid_data = std::chrono::steady_clock::now();
    bool no_data_warned = false;

    while (running_)
    {
        // ── ensure transport is open ─────────────────────────────────────────
        if (!transport_->isOpen())
        {
            LOG_DBG(MOD, "Transport not open — connecting (%s)...",
                    cfg_.sensor.sensor_device.transport.type.c_str());
            if (!transport_->open())
            {
                LOG_ERR(MOD,
                        "Transport connect failed — retry in %ds", reconnect_s);
                std::unique_lock<std::mutex> lock(cv_mutex_);
                cv_.wait_for(lock, std::chrono::seconds(reconnect_s),
                             [this]
                             { return !running_.load(); });
                continue;
            }
            LOG_INF(MOD, "Transport connected");
            // Reset data-timeout window from this connection point.
            last_valid_data = std::chrono::steady_clock::now();
            no_data_warned = false;
            if (cfg_.sensor.send_init_on_reconnect)
                init_sent = false;
        }

        // ── send init commands once per connection ────────────────────────────
        if (!init_sent)
        {
            sendInitCommands();
            init_sent = true;
        }

        if (!running_)
            break;

        // ── read one ASCII line from device ───────────────────────────────────
        std::string line = transport_->readLine(timeout_ms);

        if (line.empty())
        {
            if (!transport_->isOpen())
            {
                LOG_WRN(MOD, "Transport connection lost");
                no_data_warned = false;
            }
            else
            {
                // Warn once if no valid data has arrived within the configured window.
                double elapsed = std::chrono::duration<double>(
                                     std::chrono::steady_clock::now() - last_valid_data)
                                     .count();
                if (elapsed > data_timeout && !no_data_warned)
                {
                    LOG_WRN(MOD,
                            "No valid data received for %.1fs (timeout=%.1fs)",
                            elapsed, data_timeout);
                    no_data_warned = true;
                }
            }
            continue;
        }

        // ── parse SVP packet ──────────────────────────────────────────────────
        packets_received_++;
        SensorData data;
        if (SensorParser::parse(line, data))
        {
            {
                std::lock_guard<std::mutex> lock(snapshot_mutex_);
                snapshot_.data = data;
                snapshot_.timestamp = std::chrono::system_clock::now();
                snapshot_.is_valid = true;
                snapshot_.packets_received = packets_received_;
                snapshot_.parse_errors = parse_errors_;
            }
            last_valid_data = std::chrono::steady_clock::now();
            no_data_warned = false;
            LOG_DBG(MOD,
                    "Parsed P1=%.3f P2=%.3f Pn=%.3f",          
                    data.parameter_1_value,
                    data.parameter_2_value,
                    data.parameter_n_value);
        }
        else
        {
            parse_errors_++;
            LOG_ERR(MOD,
                    "Parse error #%llu on: %s",
                    (unsigned long long)parse_errors_, line.c_str());
        }
    }

    LOG_INF("SensorManager::receiveLoop", "Stopped");
}

// ── publish loop ──────────────────────────────────────────────────────────────

void SensorManager::publishLoop()
{
    LOG_INF("SensorManager::publishLoop", "Started");

    const auto publish_interval = std::chrono::milliseconds(cfg_.sensor.publish_interval_ms);
    const auto status_interval = std::chrono::seconds(cfg_.sensor.status_interval_sec);
    auto last_publish = std::chrono::steady_clock::now();
    auto last_status = std::chrono::steady_clock::now();

    while (running_)
    {
        // ── wait up to publish_interval, wake early on stop() ─────────────────
        {
            std::unique_lock<std::mutex> lock(cv_mutex_);
            cv_.wait_for(lock, publish_interval, [this]
                         { return !running_.load(); });
        }
        if (!running_)
            break;

        auto now = std::chrono::steady_clock::now();

        // ── publish SVP data ──────────────────────────────────────────────────
        if (now - last_publish >= publish_interval)
        {
            SensorSnapshot snap;
            {
                std::lock_guard<std::mutex> lock(snapshot_mutex_);
                snap = snapshot_;
            }

            if (!mqtt_)
            {
                LOG_DBG(MOD, "MQTT disabled — skipping data publish");
            }
            else if (snap.is_valid)
            {
                double age = epochNow() - epochOf(snap.timestamp);
                if (age < cfg_.sensor.data_timeout_sec)
                {
                    double ts = epochOf(snap.timestamp);
                    std::string payload = SensorParser::toJSON(snap.data, ts);
                    LOG_DBG("SensorManager::publishLoop", "PUB [%s] (%zu B): %s",
                            cfg_.mqtt.topics.sensor.c_str(), payload.size(), payload.c_str());
                    if (!mqtt_->publish(cfg_.mqtt.topics.sensor, payload))
                        LOG_WRN("SensorManager::publishLoop",
                                "Publish to [%s] failed", cfg_.mqtt.topics.sensor.c_str());
                }
                else
                {
                    LOG_WRN(MOD,
                            "Data stale (%.1fs > %.1fs) — skipping publish to [%s]",
                            age, cfg_.sensor.data_timeout_sec, cfg_.mqtt.topics.sensor.c_str());
                }
            }
            else
            {
                LOG_DBG(MOD, "No valid snapshot yet — skipping publish");
            }

            last_publish = now;
        }

        // ── publish status + periodic debug log ───────────────────────────────
        if (now - last_status >= status_interval)
        {
            bool connected = transport_->isOpen();
            bool data_valid = false;
            {
                std::lock_guard<std::mutex> lock(snapshot_mutex_);
                data_valid = snapshot_.is_valid;
            }

            LOG_DBG(MOD,
                    "Status: transport=%s mqtt=%s data_valid=%s "
                    "packets=%llu parse_errors=%llu",
                    connected ? "up" : "down",
                    (mqtt_ && mqtt_->isConnected()) ? "up" : "down",
                    data_valid ? "yes" : "no",
                    (unsigned long long)packets_received_,
                    (unsigned long long)parse_errors_);

            if (mqtt_)
            {
                std::string status = formatStatusJson();
                LOG_DBG(MOD, "PUB [%s] (%zu B): %s",
                        cfg_.mqtt.topics.status.c_str(), status.size(), status.c_str());
                if (!mqtt_->publish(cfg_.mqtt.topics.status, status))
                    LOG_WRN(MOD,
                            "Status publish to [%s] failed", cfg_.mqtt.topics.status.c_str());
            }

            last_status = now;
        }
    }

    LOG_INF("SensorManager::publishLoop", "Stopped");
}

// ── JSON helpers ──────────────────────────────────────────────────────────────

std::string SensorManager::formatDataJson(const SensorSnapshot &snap) const
{
    double ts = epochOf(snap.timestamp);
    return SensorParser::toJSON(snap.data, ts);
}

std::string SensorManager::formatStatusJson() const
{
    SensorSnapshot snap;
    {
        std::lock_guard<std::mutex> lock(snapshot_mutex_);
        snap = snapshot_;
    }

    double now_epoch = epochNow();
    double last_data_ts = snap.is_valid ? epochOf(snap.timestamp) : -1.0;
    double data_age_sec = snap.is_valid ? (now_epoch - last_data_ts) : -1.0;
    bool connected = transport_->isOpen();
    bool data_fresh = snap.is_valid && data_age_sec >= 0.0 && data_age_sec < cfg_.sensor.data_timeout_sec;

    const char *health;
    if (!connected)
        health = "disconnected";
    else if (!snap.is_valid)
        health = "no_data";
    else if (!data_fresh)
        health = "stale";
    else
        health = "ok";

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(3);
    oss << "{"
        << "\"ts\":" << now_epoch
        << ",\"connected\":" << (connected ? "true" : "false")
        << ",\"data_valid\":" << (snap.is_valid ? "true" : "false")
        << ",\"last_data_ts\":" << last_data_ts
        << ",\"data_age_sec\":" << std::setprecision(2) << data_age_sec
        << ",\"packets_received\":" << packets_received_
        << ",\"parse_errors\":" << parse_errors_
        << ",\"health\":\"" << health << "\""
        << "}";
    return oss.str();
}
