#include "MqttClient.hpp"
#include "Logger.hpp"
#include <mosquitto.h>
#include <stdexcept>

#define MOD "MqttClient"

// One-time library init/cleanup tied to static storage duration.
namespace {
struct MosqLib {
    MosqLib()  { mosquitto_lib_init(); }
    ~MosqLib() { mosquitto_lib_cleanup(); }
} lib_guard;
} // namespace

void MqttClient::onDisconnect(struct mosquitto *, void *userdata, int rc)
{
    auto* self = static_cast<MqttClient *>(userdata);
    self->connected_ = false;
    if (rc == 0)
        LOG_INF(MOD, "Disconnected from %s:%d (graceful)",
                self->cfg_.broker.c_str(), self->cfg_.port);
    else
        LOG_WRN(MOD, "Unexpected disconnect from %s:%d (rc=%d: %s)",
                self->cfg_.broker.c_str(), self->cfg_.port,
                rc, mosquitto_strerror(rc));
}

MqttClient::MqttClient(const MqttConfig &cfg) : cfg_(cfg)
{
    LOG_DBG(MOD, "Creating client (id=%s, broker=%s:%d)",
            cfg_.client_id.c_str(), cfg_.broker.c_str(), cfg_.port);
    mosq_ = mosquitto_new(cfg_.client_id.c_str(), true, this);
    if (!mosq_)
        throw std::runtime_error("[MqttClient] mosquitto_new() failed (out of memory)");
    mosquitto_disconnect_callback_set(mosq_, onDisconnect);
}

MqttClient::~MqttClient()
{
    disconnect();
    mosquitto_destroy(mosq_);
}

bool MqttClient::connect()
{
    LOG_DBG(MOD, "Connecting to broker %s:%d (keepalive=%ds, qos=%d)",
            cfg_.broker.c_str(), cfg_.port, cfg_.keepalive, cfg_.qos);
    int rc = mosquitto_connect(mosq_, cfg_.broker.c_str(), cfg_.port, cfg_.keepalive);
    if (rc != MOSQ_ERR_SUCCESS) {
        LOG_ERR(MOD, "Connect to %s:%d failed: %s",
                cfg_.broker.c_str(), cfg_.port, mosquitto_strerror(rc));
        return false;
    }
    mosquitto_loop_start(mosq_);   // background network thread
    connected_ = true;
    LOG_INF(MOD, "Connected to %s:%d", cfg_.broker.c_str(), cfg_.port);
    return true;
}

void MqttClient::disconnect()
{
    if (connected_) {
        LOG_INF(MOD, "Disconnecting from %s:%d", cfg_.broker.c_str(), cfg_.port);
        mosquitto_disconnect(mosq_);
        mosquitto_loop_stop(mosq_, false);
        connected_ = false;
    }
}

bool MqttClient::isConnected() const
{
    return connected_;
}

bool MqttClient::publish(const std::string &topic, const std::string &payload)
{
    if (!isConnected()) {
        LOG_WRN(MOD, "Not connected — attempting reconnect before publish to [%s]",
                topic.c_str());
        if (!reconnect())
            return false;
    }

    LOG_DBG(MOD, "PUB [%s] (%zu B): %s",
            topic.c_str(), payload.size(), payload.c_str());
    int rc = mosquitto_publish(mosq_, nullptr,
                               topic.c_str(),
                               static_cast<int>(payload.size()),
                               payload.c_str(),
                               cfg_.qos, cfg_.retain);
    if (rc != MOSQ_ERR_SUCCESS) {
        LOG_ERR(MOD, "Publish to [%s] failed: %s",
                topic.c_str(), mosquitto_strerror(rc));
        return false;
    }
    return true;
}

bool MqttClient::reconnect()
{
    LOG_WRN(MOD, "Reconnecting to %s:%d...", cfg_.broker.c_str(), cfg_.port);
    int rc = mosquitto_reconnect(mosq_);
    if (rc == MOSQ_ERR_SUCCESS) {
        connected_ = true;
        LOG_INF(MOD, "Reconnected to %s:%d", cfg_.broker.c_str(), cfg_.port);
        return true;
    }
    LOG_ERR(MOD, "Reconnect to %s:%d failed: %s",
            cfg_.broker.c_str(), cfg_.port, mosquitto_strerror(rc));
    return false;
}
