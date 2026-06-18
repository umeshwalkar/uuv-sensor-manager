#include <csignal>
#include <atomic>
#include "Config.hpp"
#include "Logger.hpp"
#include "SensorManager.hpp"

static std::atomic<bool>  g_running{true};
static SensorManager      *g_manager = nullptr;

static void sigHandler(int sig)
{
    LOG_WRN("main", "Signal %d received — shutting down", sig);
    g_running = false;
    if (g_manager)
        g_manager->stop();
}

static void usage(const char *prog)
{
    fprintf(stderr, "Usage: %s <config.json | config.ini>\n"
                    "  Supports JSON (.json) and INI (.ini) config formats.\n"
                    "  See config/Sensor_config.json for an example.\n", prog);
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    AppConfig cfg;
    try {
        cfg = AppConfig::fromFile(argv[1]);
    } catch (const std::exception& e) {
        fprintf(stderr, "Config error: %s\n", e.what());
        return 1;
    }

    // Initialise logger before anything else so all tick values are relative
    // to this point and debug/level settings are applied globally.
    Logger::init();
    Logger::setEnabled(cfg.debug.enabled);
    if (cfg.debug.enabled) {
        if      (cfg.debug.level == "debug")         Logger::setLevel(LogLevel::DEBUG);
        else if (cfg.debug.level == "info")          Logger::setLevel(LogLevel::INFO);
        else if (cfg.debug.level == "warn"  ||
                 cfg.debug.level == "warning")       Logger::setLevel(LogLevel::WARN);
        else if (cfg.debug.level == "error")         Logger::setLevel(LogLevel::ERROR);
    }

    LOG_INF("main", "Sensor Manager starting (debug=%s level=%s)",
            cfg.debug.enabled ? "on" : "off", cfg.debug.level.c_str());
    LOG_INF("main", "  Config file  : %s", argv[1]);
    LOG_INF("main", "  MQTT broker  : %s:%d", cfg.mqtt.broker.c_str(), cfg.mqtt.port);
    LOG_INF("main", "  Sensor device   : %s", cfg.sensor.sensor_device.name.c_str());
    LOG_INF("main", "  Transport    : %s", cfg.sensor.sensor_device.transport.type.c_str());
    LOG_INF("main", "  Init cmds    : %zu", cfg.sensor.init_commands.size());
    LOG_INF("main", "  Pub interval : %d ms", cfg.sensor.publish_interval_ms);

    std::signal(SIGINT,  sigHandler);
    std::signal(SIGTERM, sigHandler);

    try {
        SensorManager manager(cfg);
        g_manager = &manager;
        manager.run();
    } catch (const std::exception& e) {
        LOG_ERR("main", "Fatal exception: %s", e.what());
        return 1;
    }

    LOG_INF("main", "Sensor Manager stopped");
    return 0;
}
