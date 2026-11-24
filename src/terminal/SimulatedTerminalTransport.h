#pragma once

#include "../transports/ITransport.h"
#include "ITerminalProtocol.h"

#include <mutex>

namespace paybridge::terminal
{

class SimulatedTerminalTransport : public transport::ITransport
{
public:
    explicit SimulatedTerminalTransport(SimulatedTerminalOutcome configuredOutcome);

    transport::TransportResult open() override;
    void close() noexcept override;
    bool isOpen() const noexcept override;

    transport::TransportResult send(const std::vector<std::uint8_t>& bytes) override;
    transport::TransportBytes receive(std::size_t maximumBytes) override;

    void setReceiveTimeoutMilliseconds(std::uint32_t milliseconds) noexcept override;
    void setSendTimeoutMilliseconds(std::uint32_t milliseconds) noexcept override;

    std::string description() const override;

private:
    mutable std::mutex mutex;
    bool openState = false;
    bool hasSentRequest = false;
    bool disconnectedAfterReceive = false;
    std::uint32_t receiveTimeoutMilliseconds = 10000;
    std::uint32_t sendTimeoutMilliseconds = 10000;
    SimulatedTerminalOutcome configuredOutcome;
};

} // namespace paybridge::terminal
