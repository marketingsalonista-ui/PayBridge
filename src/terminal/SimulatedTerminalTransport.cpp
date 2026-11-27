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
        pendingResponse.clear();
        pendingResponseOffset = 0;
        responseReady = false;
    }
    return result;
}

void SimulatedTerminalTransport::close() noexcept
{
    std::lock_guard<std::mutex> lock(mutex);
    openState = false;
    hasSentRequest = false;
    lastRequest.clear();
    pendingResponse.clear();
    pendingResponseOffset = 0;
    responseReady = false;
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
    pendingResponse.clear();
    pendingResponseOffset = 0;
    responseReady = false;
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
    if (maximumBytes == 0)
        return {{false, transport::TransportError::INVALID_ARGUMENT, "SIMULATED receive size is zero"}, {}};

    // Generate and cache the complete response only once per request.
    if (!responseReady)
    {
        auto result = peer->respond(lastRequest, 65540);
        if (!result.result.success)
        {
            if (result.result.error == transport::TransportError::DISCONNECTED)
                openState = false;
            return result;
        }

        if (result.data.empty())
            return {{false, transport::TransportError::MALFORMED_RESPONSE,
                "SIMULATED peer returned an empty response"}, {}};

        pendingResponse = std::move(result.data);
        pendingResponseOffset = 0;
        responseReady = true;
    }

    if (pendingResponseOffset >= pendingResponse.size())
        return {{false, transport::TransportError::DISCONNECTED,
            "SIMULATED response has been fully consumed"}, {}};

    const std::size_t remaining = pendingResponse.size() - pendingResponseOffset;
    const std::size_t count = (maximumBytes < remaining) ? maximumBytes : remaining;

    std::vector<std::uint8_t> chunk(
        pendingResponse.begin() + pendingResponseOffset,
        pendingResponse.begin() + pendingResponseOffset + count);

    pendingResponseOffset += count;

    return {{true, transport::TransportError::NONE,
        "SIMULATED response bytes received"}, std::move(chunk)};
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
