#pragma once

#include "ITerminalProtocol.h"

#include <string>
#include <optional>

namespace paybridge::terminal
{

class SimulatedTerminalProtocol : public ITerminalProtocol
{
public:
    static std::vector<std::uint8_t> encodeRequest(
        const SimulatedTerminalRequest& request);

    static std::optional<SimulatedTerminalRequest> decodeRequest(
        const std::vector<std::uint8_t>& payload);

    static std::vector<std::uint8_t> encodeResponse(
        const SimulatedTerminalRequest& request,
        SimulatedTerminalOutcome outcome);

    bool validateRequest(const SimulatedTerminalRequest& request,
        const std::vector<std::uint8_t>& payload) const override;

    bool validateResponse(
        const std::vector<std::uint8_t>& payload,
        const SimulatedTerminalRequest& expectedRequest) const override;

    static SimulatedTerminalResponse parseResponse(
        const std::vector<std::uint8_t>& payload,
        const SimulatedTerminalRequest& expectedRequest
    );

    static std::optional<SimulatedTerminalResponse> decodeResponse(
        const std::vector<std::uint8_t>& payload);
};

} // namespace paybridge::terminal
