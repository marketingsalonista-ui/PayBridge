#pragma once

#include "../transports/ITransport.h"
#include "ITerminalProtocol.h"
#include "SimulatedTerminalPeer.h"

#include <mutex>

namespace paybridge::terminal
{

class SimulatedTerminalTransport : public transport::ITransport
{
public:
    explicit SimulatedTerminalTransport(SimulatedTerminalOutcome configuredOutcome);
    explicit SimulatedTerminalTransport(std::shared_ptr<SimulatedTerminalPeer> peer);

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
    std::uint32_t receiveTimeoutMilliseconds = 10000;
    std::uint32_t sendTimeoutMilliseconds = 10000;
    std::shared_ptr<SimulatedTerminalPeer> peer;
    std::vector<std::uint8_t> lastRequest;
};

} // namespace paybridge::terminal
