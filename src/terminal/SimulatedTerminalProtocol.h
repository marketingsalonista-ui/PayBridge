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

    // Internal simulator response framing: 4-byte big-endian payload length.
    static std::vector<std::uint8_t> framePayload(
        const std::vector<std::uint8_t>& payload);

    static std::optional<std::size_t> decodeFrameLength(
        const std::vector<std::uint8_t>& header);

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
