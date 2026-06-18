#include "Transport.hpp"
#include "Logger.hpp"
#include <stdexcept>
#include <cstring>
#include <cerrno>
#include <algorithm>
#include <chrono>
#include <thread>

// POSIX headers
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <poll.h>
#include <fcntl.h>
#include <termios.h>

#define MOD_TCPCLIENT "TcpClientTransport"
#define MOD_UDPSERVER "UdpServerTransport"
#define MOD_TCPSERVER "TcpServerTransport"
#define MOD_SERIAL    "SerialTransport"

// ── helpers ───────────────────────────────────────────────────────────────────

static const char* strerr() { return std::strerror(errno); }

static void setNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static bool pollReadable(int fd, int timeout_ms) {
    struct pollfd pfd = { fd, POLLIN, 0 };
    return poll(&pfd, 1, timeout_ms) > 0;
}

static void splitIntoLines(const char* buf, ssize_t n, std::deque<std::string>& out) {
    std::string data(buf, n);
    size_t pos = 0;
    while (pos < data.size()) {
        size_t nl = data.find('\n', pos);
        if (nl == std::string::npos) {
            out.push_back(data.substr(pos));
            break;
        }
        std::string line = data.substr(pos, nl - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) out.push_back(std::move(line));
        pos = nl + 1;
    }
}

// Strip trailing CR/LF for display in log messages.
static std::string trimCRLF(const std::string& s) {
    size_t end = s.size();
    while (end > 0 && (s[end-1] == '\r' || s[end-1] == '\n')) --end;
    return s.substr(0, end);
}

// ── UdpServerTransport ────────────────────────────────────────────────────────

UdpServerTransport::UdpServerTransport(const TransportConfig& cfg) : cfg_(cfg) {}

UdpServerTransport::~UdpServerTransport() { close(); }

bool UdpServerTransport::open() {
    LOG_DBG(MOD_UDPSERVER, "Opening UDP server on %s:%d",
            cfg_.bind_host.c_str(), cfg_.bind_port);

    fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) {
        LOG_ERR(MOD_UDPSERVER, "socket() failed: '%s'", strerr());
        return false;
    }

    int yes = 1;
    setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(static_cast<uint16_t>(cfg_.bind_port));
    inet_pton(AF_INET, cfg_.bind_host.c_str(), &addr.sin_addr);

    if (bind(fd_, (sockaddr*)&addr, sizeof(addr)) < 0) {
        LOG_ERR(MOD_UDPSERVER, "bind() failed on %s:%d: '%s'",
                cfg_.bind_host.c_str(), cfg_.bind_port, strerr());
        ::close(fd_); fd_ = -1;
        return false;
    }
    setNonBlocking(fd_);
    LOG_INF(MOD_UDPSERVER, "UDP server listening on %s:%d",
            cfg_.bind_host.c_str(), cfg_.bind_port);
    return true;
}

void UdpServerTransport::close() {
    if (fd_ >= 0) {
        LOG_DBG(MOD_UDPSERVER, "Closing UDP socket (fd=%d)", fd_);
        ::close(fd_); fd_ = -1;
    }
    line_buf_.clear();
    byte_buf_.clear();
}

void UdpServerTransport::drainDatagram() {
    char raw[4096];
    ssize_t n = recvfrom(fd_, raw, sizeof(raw)-1, 0, nullptr, nullptr);
    if (n <= 0) return;
    byte_buf_.insert(byte_buf_.end(), raw, raw + n);
    splitIntoLines(raw, n, line_buf_);
}

bool UdpServerTransport::send(const std::string& data) {
    if (fd_ < 0) {
        LOG_ERR(MOD_UDPSERVER, "send(): socket not open");
        return false;
    }
    if (cfg_.host.empty() || cfg_.port == 0) {
        LOG_ERR(MOD_UDPSERVER, "send(): no destination host/port configured");
        return false;
    }
    struct sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port   = htons(static_cast<uint16_t>(cfg_.port));
    inet_pton(AF_INET, cfg_.host.c_str(), &dest.sin_addr);
    LOG_DBG(MOD_UDPSERVER, "TX [%zu B] -> %s:%d: %s",
            data.size(), cfg_.host.c_str(), cfg_.port, trimCRLF(data).c_str());
    ssize_t sent = ::sendto(fd_, data.data(), data.size(), 0,
                            (sockaddr*)&dest, sizeof(dest));
    if (sent != static_cast<ssize_t>(data.size())) {
        LOG_ERR(MOD_UDPSERVER, "sendto() failed (sent %zd/%zu): '%s'",
                sent, data.size(), strerr());
        return false;
    }
    return true;
}

bool UdpServerTransport::sendBytes(const std::vector<uint8_t>& data) {
    if (fd_ < 0) {
        LOG_ERR(MOD_UDPSERVER, "sendBytes(): socket not open");
        return false;
    }
    if (cfg_.host.empty() || cfg_.port == 0) {
        LOG_ERR(MOD_UDPSERVER, "sendBytes(): no destination configured");
        return false;
    }
    struct sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port   = htons(static_cast<uint16_t>(cfg_.port));
    inet_pton(AF_INET, cfg_.host.c_str(), &dest.sin_addr);
    LOG_DBG(MOD_UDPSERVER, "TX bytes [%zu B] -> %s:%d",
            data.size(), cfg_.host.c_str(), cfg_.port);
    ssize_t sent = ::sendto(fd_, data.data(), data.size(), 0,
                            (sockaddr*)&dest, sizeof(dest));
    if (sent != static_cast<ssize_t>(data.size())) {
        LOG_ERR(MOD_UDPSERVER, "sendto() bytes failed (sent %zd/%zu): '%s'",
                sent, data.size(), strerr());
        return false;
    }
    return true;
}

std::string UdpServerTransport::readLine(int timeout_ms) {
    if (!line_buf_.empty()) {
        auto line = line_buf_.front(); line_buf_.pop_front();
        LOG_DBG(MOD_UDPSERVER, "RX: %s", line.c_str());
        return line;
    }
    if (!pollReadable(fd_, timeout_ms)) return "";
    drainDatagram();
    if (line_buf_.empty()) return "";
    auto line = line_buf_.front(); line_buf_.pop_front();
    LOG_DBG(MOD_UDPSERVER, "RX: %s", line.c_str());
    return line;
}

std::vector<uint8_t> UdpServerTransport::readBytes(size_t max_bytes, int timeout_ms) {
    if (!byte_buf_.empty()) {
        std::vector<uint8_t> result(byte_buf_.begin(),
                                    byte_buf_.begin() + std::min(max_bytes, byte_buf_.size()));
        byte_buf_.erase(byte_buf_.begin(), byte_buf_.begin() + result.size());
        LOG_DBG(MOD_UDPSERVER, "RX bytes [%zu B] (from buffer)", result.size());
        return result;
    }
    if (!pollReadable(fd_, timeout_ms)) return {};
    drainDatagram();
    std::vector<uint8_t> result(byte_buf_.begin(),
                                byte_buf_.begin() + std::min(max_bytes, byte_buf_.size()));
    byte_buf_.erase(byte_buf_.begin(), byte_buf_.begin() + result.size());
    if (!result.empty())
        LOG_DBG(MOD_UDPSERVER, "RX bytes [%zu B]", result.size());
    return result;
}

// ── TcpClientTransport ────────────────────────────────────────────────────────

TcpClientTransport::TcpClientTransport(const TransportConfig& cfg) : cfg_(cfg) {}

TcpClientTransport::~TcpClientTransport() { close(); }

bool TcpClientTransport::connect() {
    LOG_DBG(MOD_TCPCLIENT, "Connecting to %s:%d (timeout=%ds)",
            cfg_.host.c_str(), cfg_.port, cfg_.connect_timeout_sec);

    fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) {
        LOG_ERR(MOD_TCPCLIENT, "socket() failed: '%s'", strerr());
        return false;
    }

    struct addrinfo hints{}, *res;
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    std::string port_str = std::to_string(cfg_.port);
    if (getaddrinfo(cfg_.host.c_str(), port_str.c_str(), &hints, &res) != 0) {
        LOG_ERR(MOD_TCPCLIENT, "DNS resolution failed for %s: '%s'",
                cfg_.host.c_str(), strerr());
        ::close(fd_); fd_ = -1;
        return false;
    }

    setNonBlocking(fd_);
    int rc = ::connect(fd_, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);

    if (rc < 0 && errno != EINPROGRESS) {
        LOG_ERR(MOD_TCPCLIENT, "connect() to %s:%d failed: '%s'",
                cfg_.host.c_str(), cfg_.port, strerr());
        ::close(fd_); fd_ = -1;
        return false;
    }

    struct pollfd pfd = { fd_, POLLOUT, 0 };
    if (poll(&pfd, 1, cfg_.connect_timeout_sec * 1000) <= 0) {
        LOG_ERR(MOD_TCPCLIENT, "TCP connect timed out after %ds to %s:%d",
                cfg_.connect_timeout_sec, cfg_.host.c_str(), cfg_.port);
        ::close(fd_); fd_ = -1;
        return false;
    }
    int err = 0; socklen_t len = sizeof(err);
    getsockopt(fd_, SOL_SOCKET, SO_ERROR, &err, &len);
    if (err != 0) {
        LOG_ERR(MOD_TCPCLIENT, "TCP connection to %s:%d failed: '%s'",
                cfg_.host.c_str(), cfg_.port, std::strerror(err));
        ::close(fd_); fd_ = -1;
        return false;
    }

    LOG_INF(MOD_TCPCLIENT, "Connected to %s:%d", cfg_.host.c_str(), cfg_.port);
    return true;
}

void TcpClientTransport::disconnect() {
    if (fd_ >= 0) {
        LOG_DBG(MOD_TCPCLIENT, "Disconnected from %s:%d",
                cfg_.host.c_str(), cfg_.port);
        ::close(fd_); fd_ = -1;
    }
    recv_buf_.clear();
    byte_buf_.clear();
}

bool TcpClientTransport::open()  { return connect(); }
void TcpClientTransport::close() { disconnect(); }

std::string TcpClientTransport::readLine(int timeout_ms) {
    auto nl = recv_buf_.find('\n');
    if (nl != std::string::npos) {
        std::string line = recv_buf_.substr(0, nl);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        recv_buf_.erase(0, nl + 1);
        LOG_DBG(MOD_TCPCLIENT, "RX: %s", line.c_str());
        return line;
    }

    if (fd_ < 0) return "";   // disconnected — let the manager handle reconnect

    if (!pollReadable(fd_, timeout_ms)) return "";

    char buf[4096];
    ssize_t n = recv(fd_, buf, sizeof(buf)-1, 0);
    if (n <= 0) {
        if (n == 0)
            LOG_WRN(MOD_TCPCLIENT, "Connection closed by peer %s:%d",
                    cfg_.host.c_str(), cfg_.port);
        else
            LOG_ERR(MOD_TCPCLIENT, "recv() error from %s:%d: '%s'",
                    cfg_.host.c_str(), cfg_.port, strerr());
        disconnect();
        return "";
    }
    recv_buf_.append(buf, n);
    byte_buf_.insert(byte_buf_.end(), buf, buf + n);

    nl = recv_buf_.find('\n');
    if (nl == std::string::npos) return "";
    std::string line = recv_buf_.substr(0, nl);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    recv_buf_.erase(0, nl + 1);
    LOG_DBG(MOD_TCPCLIENT, "RX: %s", line.c_str());
    return line;
}

std::vector<uint8_t> TcpClientTransport::readBytes(size_t max_bytes, int timeout_ms) {
    if (!byte_buf_.empty()) {
        std::vector<uint8_t> result(byte_buf_.begin(),
                                    byte_buf_.begin() + std::min(max_bytes, byte_buf_.size()));
        byte_buf_.erase(byte_buf_.begin(), byte_buf_.begin() + result.size());
        LOG_DBG(MOD_TCPCLIENT, "RX bytes [%zu B] (from buffer)", result.size());
        return result;
    }

    if (fd_ < 0) return {};

    if (!pollReadable(fd_, timeout_ms)) return {};

    char buf[4096];
    ssize_t n = recv(fd_, buf, sizeof(buf)-1, 0);
    if (n <= 0) {
        if (n == 0)
            LOG_WRN(MOD_TCPCLIENT, "Connection closed by peer %s:%d",
                    cfg_.host.c_str(), cfg_.port);
        else
            LOG_ERR(MOD_TCPCLIENT, "recv() bytes error from %s:%d: '%s'",
                    cfg_.host.c_str(), cfg_.port, strerr());
        disconnect();
        return {};
    }
    byte_buf_.insert(byte_buf_.end(), buf, buf + n);
    std::vector<uint8_t> result(byte_buf_.begin(),
                                byte_buf_.begin() + std::min(max_bytes, byte_buf_.size()));
    byte_buf_.erase(byte_buf_.begin(), byte_buf_.begin() + result.size());
    LOG_DBG(MOD_TCPCLIENT, "RX bytes [%zu B]", result.size());
    return result;
}

bool TcpClientTransport::send(const std::string& data) {
    if (fd_ < 0) {
        LOG_ERR(MOD_TCPCLIENT, "send(): not connected to %s:%d",
                cfg_.host.c_str(), cfg_.port);
        return false;
    }
    LOG_DBG(MOD_TCPCLIENT, "TX [%zu B]: %s",
            data.size(), trimCRLF(data).c_str());
    ssize_t sent = ::send(fd_, data.data(), data.size(), 0);
    if (sent != static_cast<ssize_t>(data.size())) {
        LOG_ERR(MOD_TCPCLIENT, "send() to %s:%d incomplete (%zd/%zu): '%s'",
                cfg_.host.c_str(), cfg_.port, sent, data.size(), strerr());
        return false;
    }
    return true;
}

bool TcpClientTransport::sendBytes(const std::vector<uint8_t>& data) {
    if (fd_ < 0) {
        LOG_ERR(MOD_TCPCLIENT, "sendBytes(): not connected to %s:%d",
                cfg_.host.c_str(), cfg_.port);
        return false;
    }
    LOG_DBG(MOD_TCPCLIENT, "TX bytes [%zu B]", data.size());
    ssize_t sent = ::send(fd_, data.data(), data.size(), 0);
    if (sent != static_cast<ssize_t>(data.size())) {
        LOG_ERR(MOD_TCPCLIENT, "sendBytes() to %s:%d incomplete (%zd/%zu): '%s'",
                cfg_.host.c_str(), cfg_.port, sent, data.size(), strerr());
        return false;
    }
    return true;
}

// ── TcpServerTransport ────────────────────────────────────────────────────────

TcpServerTransport::TcpServerTransport(const TransportConfig& cfg) : cfg_(cfg) {}

TcpServerTransport::~TcpServerTransport() { close(); }

bool TcpServerTransport::open() {
    LOG_DBG(MOD_TCPSERVER, "Starting TCP server on %s:%d",
            cfg_.bind_host.c_str(), cfg_.bind_port);

    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        LOG_ERR(MOD_TCPSERVER, "socket() failed: '%s'", strerr());
        return false;
    }

    int yes = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(static_cast<uint16_t>(cfg_.bind_port));
    inet_pton(AF_INET, cfg_.bind_host.c_str(), &addr.sin_addr);

    if (bind(listen_fd_, (sockaddr*)&addr, sizeof(addr)) < 0) {
        LOG_ERR(MOD_TCPSERVER, "bind() on %s:%d failed: '%s'",
                cfg_.bind_host.c_str(), cfg_.bind_port, strerr());
        ::close(listen_fd_); listen_fd_ = -1;
        return false;
    }
    if (listen(listen_fd_, 1) < 0) {
        LOG_ERR(MOD_TCPSERVER, "listen() failed: '%s'", strerr());
        ::close(listen_fd_); listen_fd_ = -1;
        return false;
    }
    setNonBlocking(listen_fd_);
    LOG_INF(MOD_TCPSERVER, "TCP server listening on %s:%d — waiting for client",
            cfg_.bind_host.c_str(), cfg_.bind_port);
    return acceptClient(cfg_.connect_timeout_sec * 1000);
}

void TcpServerTransport::close() {
    if (client_fd_ >= 0) {
        LOG_DBG(MOD_TCPSERVER, "Closing client fd=%d", client_fd_);
        ::close(client_fd_); client_fd_ = -1;
    }
    if (listen_fd_ >= 0) {
        LOG_DBG(MOD_TCPSERVER, "Closing listen socket fd=%d", listen_fd_);
        ::close(listen_fd_); listen_fd_ = -1;
    }
    recv_buf_.clear();
    byte_buf_.clear();
}

bool TcpServerTransport::acceptClient(int timeout_ms) {
    LOG_DBG(MOD_TCPSERVER, "Waiting for TCP client connection (timeout=%dms)", timeout_ms);
    if (!pollReadable(listen_fd_, timeout_ms)) {
        LOG_DBG(MOD_TCPSERVER, "No client within %dms, will retry", timeout_ms);
        return false;
    }
    sockaddr_in caddr{}; socklen_t clen = sizeof(caddr);
    client_fd_ = accept(listen_fd_, (sockaddr*)&caddr, &clen);
    if (client_fd_ < 0) {
        LOG_ERR(MOD_TCPSERVER, "accept() failed: '%s'", strerr());
        return false;
    }
    setNonBlocking(client_fd_);
    char ip[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &caddr.sin_addr, ip, sizeof(ip));
    LOG_INF(MOD_TCPSERVER, "TCP client connected from %s:%d",
            ip, ntohs(caddr.sin_port));
    return true;
}

std::string TcpServerTransport::readLine(int timeout_ms) {
    if (client_fd_ < 0) {
        recv_buf_.clear();
        byte_buf_.clear();
        if (!acceptClient(timeout_ms)) return "";
    }

    auto nl = recv_buf_.find('\n');
    if (nl != std::string::npos) {
        std::string line = recv_buf_.substr(0, nl);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        recv_buf_.erase(0, nl + 1);
        LOG_DBG(MOD_TCPSERVER, "RX: %s", line.c_str());
        return line;
    }

    if (!pollReadable(client_fd_, timeout_ms)) return "";

    char buf[4096];
    ssize_t n = recv(client_fd_, buf, sizeof(buf)-1, 0);
    if (n <= 0) {
        if (n == 0)
            LOG_WRN(MOD_TCPSERVER, "TCP client disconnected");
        else
            LOG_ERR(MOD_TCPSERVER, "recv() error: '%s'", strerr());
        ::close(client_fd_); client_fd_ = -1;
        return "";
    }
    recv_buf_.append(buf, n);
    byte_buf_.insert(byte_buf_.end(), buf, buf + n);

    nl = recv_buf_.find('\n');
    if (nl == std::string::npos) return "";
    std::string line = recv_buf_.substr(0, nl);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    recv_buf_.erase(0, nl + 1);
    LOG_DBG(MOD_TCPSERVER, "RX: %s", line.c_str());
    return line;
}

std::vector<uint8_t> TcpServerTransport::readBytes(size_t max_bytes, int timeout_ms) {
    if (client_fd_ < 0) {
        byte_buf_.clear();
        if (!acceptClient(timeout_ms)) return {};
    }

    if (!byte_buf_.empty()) {
        std::vector<uint8_t> result(byte_buf_.begin(),
                                    byte_buf_.begin() + std::min(max_bytes, byte_buf_.size()));
        byte_buf_.erase(byte_buf_.begin(), byte_buf_.begin() + result.size());
        LOG_DBG(MOD_TCPSERVER, "RX bytes [%zu B] (from buffer)", result.size());
        return result;
    }

    if (!pollReadable(client_fd_, timeout_ms)) return {};

    char buf[4096];
    ssize_t n = recv(client_fd_, buf, sizeof(buf)-1, 0);
    if (n <= 0) {
        if (n == 0)
            LOG_WRN(MOD_TCPSERVER, "TCP client disconnected during byte read");
        else
            LOG_ERR(MOD_TCPSERVER, "recv() bytes error: '%s'", strerr());
        ::close(client_fd_); client_fd_ = -1;
        return {};
    }
    byte_buf_.insert(byte_buf_.end(), buf, buf + n);
    std::vector<uint8_t> result(byte_buf_.begin(),
                                byte_buf_.begin() + std::min(max_bytes, byte_buf_.size()));
    byte_buf_.erase(byte_buf_.begin(), byte_buf_.begin() + result.size());
    LOG_DBG(MOD_TCPSERVER, "RX bytes [%zu B]", result.size());
    return result;
}

bool TcpServerTransport::send(const std::string& data) {
    if (client_fd_ < 0) {
        LOG_ERR(MOD_TCPSERVER, "send(): no client connected");
        return false;
    }
    LOG_DBG(MOD_TCPSERVER, "TX [%zu B]: %s",
            data.size(), trimCRLF(data).c_str());
    ssize_t sent = ::send(client_fd_, data.data(), data.size(), 0);
    if (sent != static_cast<ssize_t>(data.size())) {
        LOG_ERR(MOD_TCPSERVER, "send() incomplete (%zd/%zu): '%s'",
                sent, data.size(), strerr());
        return false;
    }
    return true;
}

bool TcpServerTransport::sendBytes(const std::vector<uint8_t>& data) {
    if (client_fd_ < 0) {
        LOG_ERR(MOD_TCPSERVER, "sendBytes(): no client connected");
        return false;
    }
    LOG_DBG(MOD_TCPSERVER, "TX bytes [%zu B]", data.size());
    ssize_t sent = ::send(client_fd_, data.data(), data.size(), 0);
    if (sent != static_cast<ssize_t>(data.size())) {
        LOG_ERR(MOD_TCPSERVER, "sendBytes() incomplete (%zd/%zu): '%s'",
                sent, data.size(), strerr());
        return false;
    }
    return true;
}

// ── SerialTransport ───────────────────────────────────────────────────────────

SerialTransport::SerialTransport(const TransportConfig& cfg) : cfg_(cfg) {}

SerialTransport::~SerialTransport() { close(); }

bool SerialTransport::configureSerialPort() {
    struct termios tios;
    if (tcgetattr(fd_, &tios) != 0) {
        LOG_ERR(MOD_SERIAL, "tcgetattr() on %s failed: '%s'",
                cfg_.serial_port.c_str(), strerr());
        return false;
    }

    speed_t baud = B115200;
    switch (cfg_.serial_baud) {
        case 9600:   baud = B9600;   break;
        case 19200:  baud = B19200;  break;
        case 38400:  baud = B38400;  break;
        case 57600:  baud = B57600;  break;
        case 115200: baud = B115200; break;
        default:
            LOG_WRN(MOD_SERIAL, "Unsupported baud %d — defaulting to 115200",
                    cfg_.serial_baud);
            break;
    }
    cfsetispeed(&tios, baud);
    cfsetospeed(&tios, baud);

    tios.c_cflag &= ~CSIZE;
    switch (cfg_.serial_data_bits) {
        case 5: tios.c_cflag |= CS5; break;
        case 6: tios.c_cflag |= CS6; break;
        case 7: tios.c_cflag |= CS7; break;
        case 8: tios.c_cflag |= CS8; break;
    }

    if (cfg_.serial_stop_bits == 2)
        tios.c_cflag |= CSTOPB;
    else
        tios.c_cflag &= ~CSTOPB;

    tios.c_cflag &= ~(PARENB | PARODD);
    if      (cfg_.serial_parity == "E") tios.c_cflag |= PARENB;
    else if (cfg_.serial_parity == "O") tios.c_cflag |= PARENB | PARODD;

    tios.c_cflag |= CREAD;
    tios.c_cflag &= ~CRTSCTS;
    tios.c_iflag &= ~(IXON | IXOFF | IXANY);
    tios.c_lflag = 0;
    tios.c_oflag = 0;
    tios.c_cc[VMIN]  = 0;
    tios.c_cc[VTIME] = cfg_.read_timeout_ms / 100;

    if (tcsetattr(fd_, TCSANOW, &tios) != 0) {
        LOG_ERR(MOD_SERIAL, "tcsetattr() on %s failed: '%s'",
                cfg_.serial_port.c_str(), strerr());
        return false;
    }
    return true;
}

bool SerialTransport::open() {
    LOG_DBG(MOD_SERIAL, "Opening %s at %d baud",
            cfg_.serial_port.c_str(), cfg_.serial_baud);
    fd_ = ::open(cfg_.serial_port.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd_ < 0) {
        LOG_ERR(MOD_SERIAL, "Failed to open %s: '%s'",
                cfg_.serial_port.c_str(), strerr());
        return false;
    }
    if (!configureSerialPort()) {
        LOG_ERR(MOD_SERIAL, "Failed to configure %s — closing",
                cfg_.serial_port.c_str());
        ::close(fd_); fd_ = -1;
        return false;
    }
    LOG_INF(MOD_SERIAL, "Serial port %s open at %d baud %d%s%d",
            cfg_.serial_port.c_str(), cfg_.serial_baud,
            cfg_.serial_data_bits, cfg_.serial_parity.c_str(), cfg_.serial_stop_bits);
    return true;
}

void SerialTransport::close() {
    if (fd_ >= 0) {
        LOG_DBG(MOD_SERIAL, "Closing %s", cfg_.serial_port.c_str());
        ::close(fd_); fd_ = -1;
    }
    recv_buf_.clear();
    byte_buf_.clear();
}

std::string SerialTransport::readLine(int timeout_ms) {
    auto nl = recv_buf_.find('\n');
    if (nl != std::string::npos) {
        std::string line = recv_buf_.substr(0, nl);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        recv_buf_.erase(0, nl + 1);
        LOG_DBG(MOD_SERIAL, "RX: %s", line.c_str());
        return line;
    }

    if (!pollReadable(fd_, timeout_ms)) return "";

    char buf[4096];
    ssize_t n = read(fd_, buf, sizeof(buf)-1);
    if (n <= 0) {
        if (n < 0 && errno != EAGAIN)
            LOG_ERR(MOD_SERIAL, "read() on %s failed: '%s'",
                    cfg_.serial_port.c_str(), strerr());
        return "";
    }
    recv_buf_.append(buf, n);

    nl = recv_buf_.find('\n');
    if (nl == std::string::npos) return "";
    std::string line = recv_buf_.substr(0, nl);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    recv_buf_.erase(0, nl + 1);
    LOG_DBG(MOD_SERIAL, "RX: %s", line.c_str());
    return line;
}

std::vector<uint8_t> SerialTransport::readBytes(size_t max_bytes, int timeout_ms) {
    if (!byte_buf_.empty()) {
        std::vector<uint8_t> result(byte_buf_.begin(),
                                    byte_buf_.begin() + std::min(max_bytes, byte_buf_.size()));
        byte_buf_.erase(byte_buf_.begin(), byte_buf_.begin() + result.size());
        LOG_DBG(MOD_SERIAL, "RX bytes [%zu B] (from buffer)", result.size());
        return result;
    }

    if (!pollReadable(fd_, timeout_ms)) return {};

    char buf[4096];
    ssize_t n = read(fd_, buf, sizeof(buf)-1);
    if (n <= 0) {
        if (n < 0 && errno != EAGAIN)
            LOG_ERR(MOD_SERIAL, "read() bytes on %s failed: '%s'",
                    cfg_.serial_port.c_str(), strerr());
        return {};
    }
    byte_buf_.insert(byte_buf_.end(), buf, buf + n);
    std::vector<uint8_t> result(byte_buf_.begin(),
                                byte_buf_.begin() + std::min(max_bytes, byte_buf_.size()));
    byte_buf_.erase(byte_buf_.begin(), byte_buf_.begin() + result.size());
    LOG_DBG(MOD_SERIAL, "RX bytes [%zu B]", result.size());
    return result;
}

bool SerialTransport::send(const std::string& data) {
    if (fd_ < 0) {
        LOG_ERR(MOD_SERIAL, "send(): %s not open", cfg_.serial_port.c_str());
        return false;
    }
    LOG_DBG(MOD_SERIAL, "TX [%zu B]: %s",
            data.size(), trimCRLF(data).c_str());
    ssize_t written = write(fd_, data.data(), data.size());
    if (written != static_cast<ssize_t>(data.size())) {
        LOG_ERR(MOD_SERIAL, "write() on %s incomplete (%zd/%zu): '%s'",
                cfg_.serial_port.c_str(), written, data.size(), strerr());
        return false;
    }
    return true;
}

bool SerialTransport::sendBytes(const std::vector<uint8_t>& data) {
    if (fd_ < 0) {
        LOG_ERR(MOD_SERIAL, "sendBytes(): %s not open", cfg_.serial_port.c_str());
        return false;
    }
    LOG_DBG(MOD_SERIAL, "TX bytes [%zu B] on %s",
            data.size(), cfg_.serial_port.c_str());
    ssize_t written = write(fd_, data.data(), data.size());
    if (written != static_cast<ssize_t>(data.size())) {
        LOG_ERR(MOD_SERIAL, "write() bytes on %s incomplete (%zd/%zu): '%s'",
                cfg_.serial_port.c_str(), written, data.size(), strerr());
        return false;
    }
    return true;
}

// ── factory ───────────────────────────────────────────────────────────────────

std::unique_ptr<ITransport> makeTransport(const TransportConfig& cfg) {
    if (cfg.type == "udp_server")
        return std::make_unique<UdpServerTransport>(cfg);
    if (cfg.type == "tcp_client")
        return std::make_unique<TcpClientTransport>(cfg);
    if (cfg.type == "tcp_server")
        return std::make_unique<TcpServerTransport>(cfg);
    if (cfg.type == "serial")
        return std::make_unique<SerialTransport>(cfg);
    LOG_ERR("Transport", "Unknown transport type '%s'", cfg.type.c_str());
    throw std::runtime_error("Unknown transport type: " + cfg.type);
}
