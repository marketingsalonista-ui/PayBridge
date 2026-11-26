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

// Internal PayBridge simulation contract only. These records are not a
// physical terminal wire format and must not be used as vendor commands.
struct SimulatedTerminalRequest
{
    std::string requestId;
    std::string transactionId;
    std::string externalReference;
    std::string terminalId;
    std::string amount;
    std::string assetIdentity;
};

struct SimulatedTerminalResponse
{
    SimulatedTerminalOutcome outcome = SimulatedTerminalOutcome::MALFORMED;
    std::string requestId;
    std::string transactionId;
    std::string terminalId;
    std::string externalReference;
    std::string simulatedResult;
    bool simulated = true;
};

class ITerminalProtocol
{
public:
    virtual ~ITerminalProtocol() = default;

    virtual bool validateRequest(
        const SimulatedTerminalRequest& request,
        const std::vector<std::uint8_t>& payload
    ) const = 0;

    virtual bool validateResponse(
        const std::vector<std::uint8_t>& payload,
        const SimulatedTerminalRequest& expectedRequest
    ) const = 0;
};

} // namespace paybridge::terminal
