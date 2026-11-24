#pragma once

#include "ITransport.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <mutex>
#include <string>

namespace paybridge::transport
{

class TcpTransport : public ITransport
{
public:
    TcpTransport(std::string host, std::uint16_t port);
    ~TcpTransport() override;

    TransportResult open() override;
    void close() noexcept override;
    bool isOpen() const noexcept override;

    TransportResult send(const std::vector<std::uint8_t>& bytes) override;
    TransportBytes receive(std::size_t maximumBytes) override;

    void setReceiveTimeoutMilliseconds(std::uint32_t milliseconds) noexcept override;
    void setSendTimeoutMilliseconds(std::uint32_t milliseconds) noexcept override;

    std::string description() const override;

private:
    mutable std::mutex mutex;
    SOCKET socketHandle = INVALID_SOCKET;
    std::string host;
    std::uint16_t port;
    std::uint32_t receiveTimeoutMilliseconds = 10000;
    std::uint32_t sendTimeoutMilliseconds = 10000;

    bool wsaInitialized = false;
};

} // namespace paybridge::transport
