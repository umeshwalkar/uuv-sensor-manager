#pragma once
#include <string>
#include <memory>
#include <deque>
#include <vector>
#include <cstdint>
#include "Config.hpp"

/**
 * Abstract transport interface for receiving/sending data via various media
 */
class ITransport {
public:
    virtual ~ITransport() = default;
    
    // Connection management
    virtual bool        open()                       = 0;
    virtual void        close()                      = 0;
    virtual bool        isOpen() const               = 0;
    
    // Data reception (returns complete lines or packets)
    virtual std::string readLine(int timeout_ms = 100) = 0;
    
    // Raw data reception (for binary protocols like HNAV)
    virtual std::vector<uint8_t> readBytes(size_t max_bytes, int timeout_ms = 100) = 0;
    
    // Data transmission
    virtual bool        send(const std::string& data) = 0;
    virtual bool        sendBytes(const std::vector<uint8_t>& data) = 0;
};

// ============================================================================
// UDP Server Transport (receive only)
// ============================================================================

class UdpServerTransport : public ITransport {
public:
    explicit UdpServerTransport(const TransportConfig& cfg);
    ~UdpServerTransport() override;

    bool                        open()  override;
    void                        close() override;
    bool                        isOpen() const override { return fd_ >= 0; }
    std::string                 readLine(int timeout_ms = 100) override;
    std::vector<uint8_t>        readBytes(size_t max_bytes, int timeout_ms = 100) override;
    bool                        send(const std::string& data) override;
    bool                        sendBytes(const std::vector<uint8_t>& data) override;

private:
    TransportConfig             cfg_;
    int                         fd_ = -1;
    std::deque<std::string>     line_buf_;
    std::vector<uint8_t>        byte_buf_;

    void drainDatagram();
};

// ============================================================================
// TCP Client Transport (connect to remote server)
// ============================================================================

class TcpClientTransport : public ITransport {
public:
    explicit TcpClientTransport(const TransportConfig& cfg);
    ~TcpClientTransport() override;

    bool                        open()  override;
    void                        close() override;
    bool                        isOpen() const override { return fd_ >= 0; }
    std::string                 readLine(int timeout_ms = 100) override;
    std::vector<uint8_t>        readBytes(size_t max_bytes, int timeout_ms = 100) override;
    bool                        send(const std::string& data) override;
    bool                        sendBytes(const std::vector<uint8_t>& data) override;

private:
    TransportConfig             cfg_;
    int                         fd_ = -1;
    std::string                 recv_buf_;
    std::vector<uint8_t>        byte_buf_;
    int                         reconnect_countdown_ = 0;

    bool connect();
    void disconnect();
};

// ============================================================================
// TCP Server Transport (bind and accept one client)
// ============================================================================

class TcpServerTransport : public ITransport {
public:
    explicit TcpServerTransport(const TransportConfig& cfg);
    ~TcpServerTransport() override;

    bool                        open()  override;
    void                        close() override;
    bool                        isOpen() const override { return client_fd_ >= 0; }
    std::string                 readLine(int timeout_ms = 100) override;
    std::vector<uint8_t>        readBytes(size_t max_bytes, int timeout_ms = 100) override;
    bool                        send(const std::string& data) override;
    bool                        sendBytes(const std::vector<uint8_t>& data) override;

private:
    TransportConfig             cfg_;
    int                         listen_fd_  = -1;
    int                         client_fd_  = -1;
    std::string                 recv_buf_;
    std::vector<uint8_t>        byte_buf_;

    bool acceptClient(int timeout_ms);
};

// ============================================================================
// Serial RS232 Transport (bidirectional)
// ============================================================================

class SerialTransport : public ITransport {
public:
    explicit SerialTransport(const TransportConfig& cfg);
    ~SerialTransport() override;

    bool                        open()  override;
    void                        close() override;
    bool                        isOpen() const override { return fd_ >= 0; }
    std::string                 readLine(int timeout_ms = 100) override;
    std::vector<uint8_t>        readBytes(size_t max_bytes, int timeout_ms = 100) override;
    bool                        send(const std::string& data) override;
    bool                        sendBytes(const std::vector<uint8_t>& data) override;

private:
    TransportConfig             cfg_;
    int                         fd_ = -1;
    std::string                 recv_buf_;
    std::vector<uint8_t>        byte_buf_;

    bool configureSerialPort();
};

// ============================================================================
// Factory function
// ============================================================================

std::unique_ptr<ITransport> makeTransport(const TransportConfig& cfg);
