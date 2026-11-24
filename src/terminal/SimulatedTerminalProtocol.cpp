#include "SimulatedTerminalProtocol.h"

#include <algorithm>

namespace paybridge::terminal
{

bool SimulatedTerminalProtocol::validateRequest(
    const std::string& terminalId,
    const std::string& externalReference,
    const std::vector<std::uint8_t>& payload) const
{
    if (terminalId.empty() || externalReference.empty() || payload.empty())
        return false;

    if (payload.size() > 65536)
        return false;

    return true;
}

bool SimulatedTerminalProtocol::validateResponse(
    const std::vector<std::uint8_t>& payload) const
{
    const std::string text(payload.begin(), payload.end());

    return text == "SIMULATED_RESPONSE APPROVED" ||
           text == "SIMULATED_RESPONSE DECLINED";
}

SimulatedTerminalResponse SimulatedTerminalProtocol::parseResponse(
    const std::vector<std::uint8_t>& payload,
    const std::string& terminalId)
{
    const std::string text(payload.begin(), payload.end());

    SimulatedTerminalResponse response;
    response.terminalId = terminalId;

    if (text == "SIMULATED_RESPONSE APPROVED")
    {
        response.outcome = SimulatedTerminalOutcome::APPROVED;
        response.simulatedResult = "SIMULATED APPROVED - no real payment was processed";
    }
    else if (text == "SIMULATED_RESPONSE DECLINED")
    {
        response.outcome = SimulatedTerminalOutcome::DECLINED;
        response.simulatedResult = "SIMULATED DECLINED - no real payment was processed";
    }
    else
    {
        response.outcome = SimulatedTerminalOutcome::MALFORMED;
        response.simulatedResult = "INVALID SIMULATED terminal response";
    }

    return response;
}

} // namespace paybridge::terminal
