#pragma once

#include "ITerminalProtocol.h"

#include <string>

namespace paybridge::terminal
{

class SimulatedTerminalProtocol : public ITerminalProtocol
{
public:
    bool validateRequest(
        const std::string& terminalId,
        const std::string& externalReference,
        const std::vector<std::uint8_t>& payload
    ) const override;

    bool validateResponse(
        const std::vector<std::uint8_t>& payload
    ) const override;

    static SimulatedTerminalResponse parseResponse(
        const std::vector<std::uint8_t>& payload,
        const std::string& terminalId
    );
};

} // namespace paybridge::terminal
