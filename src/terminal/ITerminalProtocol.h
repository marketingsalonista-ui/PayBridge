#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace paybridge::terminal
{

// Wire-protocol-neutral simulation contract.
// No Verifone command codes or physical terminal message formats are implied.
enum class SimulatedTerminalOutcome
{
    APPROVED,
    DECLINED,
    TIMEOUT,
    DISCONNECTED,
    MALFORMED
};

struct SimulatedTerminalResponse
{
    SimulatedTerminalOutcome outcome = SimulatedTerminalOutcome::MALFORMED;
    std::string terminalId;
    std::string externalReference;
    std::string simulatedResult;
};

class ITerminalProtocol
{
public:
    virtual ~ITerminalProtocol() = default;

    virtual bool validateRequest(
        const std::string& terminalId,
        const std::string& externalReference,
        const std::vector<std::uint8_t>& payload
    ) const = 0;

    virtual bool validateResponse(
        const std::vector<std::uint8_t>& payload
    ) const = 0;
};

} // namespace paybridge::terminal
