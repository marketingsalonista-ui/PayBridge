#include "SimulatedTerminalTransport.h"

namespace paybridge::terminal
{

SimulatedTerminalTransport::SimulatedTerminalTransport(
    SimulatedTerminalOutcome outcome)
    : peer(makeSimulatedTerminalPeer(outcome))
{
}

SimulatedTerminalTransport::SimulatedTerminalTransport(
    std::shared_ptr<SimulatedTerminalPeer> simulatedPeer)
    : peer(std::move(simulatedPeer))
{
}

transport::TransportResult SimulatedTerminalTransport::open()
{
    std::lock_guard<std::mutex> lock(mutex);
    if (openState)
        return {false, transport::TransportError::ALREADY_OPEN,
            "SIMULATED transport is already open"};
    if (!peer)
        return {false, transport::TransportError::CONNECT_FAILED,
            "SIMULATED peer is not configured"};
    const auto result = peer->open();
    if (result.success)
    {
        openState = true;
        hasSentRequest = false;
        lastRequest.clear();
    }
    return result;
}

void SimulatedTerminalTransport::close() noexcept
{
    std::lock_guard<std::mutex> lock(mutex);
    openState = false;
    hasSentRequest = false;
    lastRequest.clear();
}

bool SimulatedTerminalTransport::isOpen() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex);
    return openState;
}

transport::TransportResult SimulatedTerminalTransport::send(
    const std::vector<std::uint8_t>& bytes)
{
    std::lock_guard<std::mutex> lock(mutex);
    if (!openState)
        return {false, transport::TransportError::NOT_OPEN, "SIMULATED transport is not open"};
    if (bytes.empty())
        return {false, transport::TransportError::INVALID_ARGUMENT, "SIMULATED request is empty"};
    if (bytes.size() > 65536)
        return {false, transport::TransportError::SIZE_LIMIT, "SIMULATED request exceeds size limit"};
    lastRequest = bytes;
    hasSentRequest = true;
    return {true, transport::TransportError::NONE, "SIMULATED bytes accepted"};
}

transport::TransportBytes SimulatedTerminalTransport::receive(std::size_t maximumBytes)
{
    std::lock_guard<std::mutex> lock(mutex);
    if (!openState)
        return {{false, transport::TransportError::NOT_OPEN, "SIMULATED transport is not open"}, {}};
    if (!hasSentRequest)
        return {{false, transport::TransportError::INVALID_ARGUMENT, "No SIMULATED request was sent"}, {}};
    if (!peer)
        return {{false, transport::TransportError::DISCONNECTED, "SIMULATED peer unavailable"}, {}};
    auto result = peer->respond(lastRequest, maximumBytes);
    if (result.result.error == transport::TransportError::DISCONNECTED)
        openState = false;
    return result;
}

void SimulatedTerminalTransport::setReceiveTimeoutMilliseconds(std::uint32_t milliseconds) noexcept
{
    std::lock_guard<std::mutex> lock(mutex);
    receiveTimeoutMilliseconds = milliseconds;
}

void SimulatedTerminalTransport::setSendTimeoutMilliseconds(std::uint32_t milliseconds) noexcept
{
    std::lock_guard<std::mutex> lock(mutex);
    sendTimeoutMilliseconds = milliseconds;
}

std::string SimulatedTerminalTransport::description() const
{
    return "SIMULATED byte transport; protocol handled by simulator adapter";
}

} // namespace paybridge::terminal
