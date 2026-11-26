#pragma once

#include "ITerminalProtocol.h"
#include "../transports/TransportResult.h"

#include <cstddef>
#include <memory>
#include <vector>

namespace paybridge::terminal
{

// In-process terminal endpoint for tests and simulator use. Protocol parsing
// and response construction live here, outside the byte transport.
class SimulatedTerminalPeer
{
public:
    explicit SimulatedTerminalPeer(SimulatedTerminalOutcome outcome);

    transport::TransportResult open() const;
    transport::TransportBytes respond(
        const std::vector<std::uint8_t>& request,
        std::size_t maximumBytes) const;

private:
    SimulatedTerminalOutcome configuredOutcome;
};

std::shared_ptr<SimulatedTerminalPeer> makeSimulatedTerminalPeer(
    SimulatedTerminalOutcome outcome);

} // namespace paybridge::terminal
