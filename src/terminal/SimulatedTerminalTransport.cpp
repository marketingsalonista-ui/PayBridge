#include "SimulatedTerminalTransport.h"
#include <utility>

#include <cstring>

namespace paybridge::terminal
{

SimulatedTerminalTransport::SimulatedTerminalTransport(
    SimulatedTerminalOutcome configuredOutcome)
    : configuredOutcome(configuredOutcome)
{
}

transport::TransportResult SimulatedTerminalTransport::open()
{
    std::lock_guard<std::mutex> lock(mutex);

    if (openState)
        return {false, transport::TransportError::ALREADY_OPEN,
                "Simulated transport is already open"};

    if (configuredOutcome == SimulatedTerminalOutcome::DISCONNECTED)
        return {false, transport::TransportError::CONNECT_FAILED,
                "SIMULATED terminal connection failed"};

    openState = true;
    hasSentRequest = false;
    disconnectedAfterReceive = false;
    return {true, transport::TransportError::NONE,
            "SIMULATED terminal connected"};
}

void SimulatedTerminalTransport::close() noexcept
{
    std::lock_guard<std::mutex> lock(mutex);
    openState = false;
    hasSentRequest = false;
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
        return {false, transport::TransportError::NOT_OPEN,
                "SIMULATED terminal is not open"};

    if (bytes.empty())
        return {false, transport::TransportError::INVALID_ARGUMENT,
                "SIMULATED terminal request is empty"};

    if (bytes.size() > 65536)
        return {false, transport::TransportError::SIZE_LIMIT,
                "SIMULATED terminal request exceeds size limit"};

    hasSentRequest = true;
    return {true, transport::TransportError::NONE,
            "SIMULATED terminal request sent"};
}

transport::TransportBytes SimulatedTerminalTransport::receive(std::size_t maximumBytes)
{
    std::lock_guard<std::mutex> lock(mutex);

    if (!openState)
    {
        return {{false, transport::TransportError::NOT_OPEN,
                 "SIMULATED terminal is not open"}, {}};
    }

    if (!hasSentRequest)
    {
        return {{false, transport::TransportError::INVALID_ARGUMENT,
                 "No SIMULATED terminal request was sent"}, {}};
    }

    if (maximumBytes == 0)
    {
        return {{false, transport::TransportError::INVALID_ARGUMENT,
                 "Maximum receive size is zero"}, {}};
    }

    switch (configuredOutcome)
    {
        case SimulatedTerminalOutcome::APPROVED:
        {
            static const std::string text =
                "SIMULATED_RESPONSE APPROVED";
            std::vector<std::uint8_t> result(text.begin(), text.end());
            if (result.size() > maximumBytes)
                return {{false, transport::TransportError::SIZE_LIMIT,
                         "SIMULATED response exceeds receive limit"}, {}};
            return {{true, transport::TransportError::NONE,
                     "SIMULATED approved response"}, std::move(result)};
        }

        case SimulatedTerminalOutcome::DECLINED:
        {
            static const std::string text =
                "SIMULATED_RESPONSE DECLINED";
            std::vector<std::uint8_t> result(text.begin(), text.end());
            if (result.size() > maximumBytes)
                return {{false, transport::TransportError::SIZE_LIMIT,
                         "SIMULATED response exceeds receive limit"}, {}};
            return {{true, transport::TransportError::NONE,
                     "SIMULATED declined response"}, std::move(result)};
        }

        case SimulatedTerminalOutcome::TIMEOUT:
            return {{false, transport::TransportError::TIMEOUT,
                     "SIMULATED terminal response timed out"}, {}};

        case SimulatedTerminalOutcome::DISCONNECTED:
            disconnectedAfterReceive = true;
            openState = false;
            return {{false, transport::TransportError::DISCONNECTED,
                     "SIMULATED terminal disconnected"}, {}};

        case SimulatedTerminalOutcome::MALFORMED:
        {
            static const std::string text = "%%% MALFORMED-SIMULATION %%%";
            std::vector<std::uint8_t> result(text.begin(), text.end());
            if (result.size() > maximumBytes)
                return {{false, transport::TransportError::SIZE_LIMIT,
                         "SIMULATED malformed response exceeds receive limit"}, {}};
            return {{true, transport::TransportError::NONE,
                     "SIMULATED malformed response"}, std::move(result)};
        }
    }

    return {{false, transport::TransportError::MALFORMED_RESPONSE,
             "Unknown SIMULATED terminal outcome"}, {}};
}

void SimulatedTerminalTransport::setReceiveTimeoutMilliseconds(
    std::uint32_t milliseconds) noexcept
{
    std::lock_guard<std::mutex> lock(mutex);
    receiveTimeoutMilliseconds = milliseconds;
}

void SimulatedTerminalTransport::setSendTimeoutMilliseconds(
    std::uint32_t milliseconds) noexcept
{
    std::lock_guard<std::mutex> lock(mutex);
    sendTimeoutMilliseconds = milliseconds;
}

std::string SimulatedTerminalTransport::description() const
{
    return "SIMULATED Verifone VX 680 profile - no physical protocol";
}

} // namespace paybridge::terminal
