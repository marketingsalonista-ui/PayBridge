#pragma once

#include "TerminalModel.h"
#include "ITerminalProtocol.h"
#include "../transports/ITransport.h"

#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace paybridge::terminal
{

class TerminalManager
{
public:
    void registerTerminal(const TerminalModel& terminal);
    bool removeTerminal(const std::string& terminalId);
    bool containsTerminal(const std::string& terminalId) const;
    std::vector<TerminalModel> terminals() const;

    void setTransport(
        const std::string& terminalId,
        std::unique_ptr<transport::ITransport> transport
    );

    transport::TransportResult connect(const std::string& terminalId);
    void disconnect(const std::string& terminalId);

    transport::TransportBytes sendAndReceive(
        const std::string& terminalId,
        const std::vector<std::uint8_t>& request,
        std::size_t maximumResponseBytes
    );

private:
    mutable std::mutex mutex;
    std::unordered_map<std::string, TerminalModel> terminalModels;
    std::unordered_map<std::string, std::unique_ptr<transport::ITransport>> transports;
};

} // namespace paybridge::terminal
