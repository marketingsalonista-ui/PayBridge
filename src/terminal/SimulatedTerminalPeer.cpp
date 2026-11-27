#include "SimulatedTerminalPeer.h"

#include "SimulatedTerminalProtocol.h"

namespace paybridge::terminal
{

SimulatedTerminalPeer::SimulatedTerminalPeer(SimulatedTerminalOutcome outcome)
    : configuredOutcome(outcome)
{
}

transport::TransportResult SimulatedTerminalPeer::open() const
{
    if (configuredOutcome == SimulatedTerminalOutcome::DISCONNECTED)
        return {false, transport::TransportError::CONNECT_FAILED,
            "SIMULATED terminal connection failed"};
    return {true, transport::TransportError::NONE,
        "SIMULATED terminal peer connected"};
}

transport::TransportBytes SimulatedTerminalPeer::respond(
    const std::vector<std::uint8_t>& requestBytes,
    std::size_t maximumBytes) const
{
    if (maximumBytes == 0)
        return {{false, transport::TransportError::INVALID_ARGUMENT,
            "Maximum receive size is zero"}, {}};

    switch (configuredOutcome)
    {
    case SimulatedTerminalOutcome::APPROVED:
    case SimulatedTerminalOutcome::DECLINED:
    {
        const auto request = SimulatedTerminalProtocol::decodeRequest(requestBytes);
        if (!request)
            return {{false, transport::TransportError::MALFORMED_RESPONSE,
                "Invalid SIMULATED request contract"}, {}};
        auto response = SimulatedTerminalProtocol::encodeResponse(*request, configuredOutcome);
        if (response.empty())
            return {{false, transport::TransportError::MALFORMED_RESPONSE,
                "Could not encode SIMULATED response"}, {}};
        if (response.size() > maximumBytes)
            return {{false, transport::TransportError::SIZE_LIMIT,
                "SIMULATED response exceeds receive limit"}, {}};
        return {{true, transport::TransportError::NONE,
            configuredOutcome == SimulatedTerminalOutcome::APPROVED
                ? "SIMULATED approved response" : "SIMULATED declined response"},
            std::move(response)};
    }
    case SimulatedTerminalOutcome::TIMEOUT:
        return {{false, transport::TransportError::TIMEOUT,
            "SIMULATED terminal response timed out"}, {}};
    case SimulatedTerminalOutcome::DISCONNECTED:
        return {{false, transport::TransportError::DISCONNECTED,
            "SIMULATED terminal disconnected"}, {}};
    case SimulatedTerminalOutcome::MALFORMED:
    {
        static const std::string malformed = "%%% MALFORMED-SIMULATION %%%";
        const std::vector<std::uint8_t> payload(malformed.begin(), malformed.end());
        const auto frame = SimulatedTerminalProtocol::framePayload(payload);
        if (frame.empty() || frame.size() > maximumBytes)
            return {{false, transport::TransportError::SIZE_LIMIT,
                "SIMULATED malformed response exceeds receive limit"}, {}};
        return {{true, transport::TransportError::NONE,
            "SIMULATED malformed response"}, frame};
    }
    }
    return {{false, transport::TransportError::MALFORMED_RESPONSE,
        "Unknown SIMULATED terminal outcome"}, {}};
}

std::shared_ptr<SimulatedTerminalPeer> makeSimulatedTerminalPeer(
    SimulatedTerminalOutcome outcome)
{
    return std::make_shared<SimulatedTerminalPeer>(outcome);
}

} // namespace paybridge::terminal
