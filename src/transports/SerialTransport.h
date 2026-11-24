#pragma once

#include "ITransport.h"

#include <windows.h>
#include <mutex>
#include <string>

namespace paybridge::transport
{

class SerialTransport : public ITransport
{
public:
    SerialTransport(std::string portName, std::uint32_t baudRate = 115200);
    ~SerialTransport() override;

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
    HANDLE handle = INVALID_HANDLE_VALUE;
    std::string portName;
    std::uint32_t baudRate;
    std::uint32_t receiveTimeoutMilliseconds = 10000;
    std::uint32_t sendTimeoutMilliseconds = 10000;

    void configureTimeOuts();
};

} // namespace paybridge::transport
