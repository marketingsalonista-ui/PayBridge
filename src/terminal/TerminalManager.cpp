#include "TerminalManager.h"

namespace paybridge::terminal
{

void TerminalManager::registerTerminal(const TerminalModel& terminal)
{
    if (terminal.terminalId.empty())
        return;

    std::lock_guard<std::mutex> lock(mutex);
    terminalModels[terminal.terminalId] = terminal;
}

bool TerminalManager::removeTerminal(const std::string& terminalId)
{
    std::lock_guard<std::mutex> lock(mutex);

    const auto transport = transports.find(terminalId);
    if (transport != transports.end())
        transport->second->close();

    transports.erase(terminalId);
    return terminalModels.erase(terminalId) != 0;
}

bool TerminalManager::containsTerminal(const std::string& terminalId) const
{
    std::lock_guard<std::mutex> lock(mutex);
    return terminalModels.find(terminalId) != terminalModels.end();
}

std::vector<TerminalModel> TerminalManager::terminals() const
{
    std::lock_guard<std::mutex> lock(mutex);

    std::vector<TerminalModel> result;
    result.reserve(terminalModels.size());

    for (const auto& item : terminalModels)
        result.push_back(item.second);

    return result;
}

void TerminalManager::setTransport(
    const std::string& terminalId,
    std::unique_ptr<transport::ITransport> transport)
{
    if (terminalId.empty() || !transport)
        return;

    std::lock_guard<std::mutex> lock(mutex);

    if (terminalModels.find(terminalId) == terminalModels.end())
        return;

    transports[terminalId] = std::move(transport);
}

transport::TransportResult TerminalManager::connect(const std::string& terminalId)
{
    std::lock_guard<std::mutex> lock(mutex);

    const auto model = terminalModels.find(terminalId);
    if (model == terminalModels.end())
        return {false, transport::TransportError::INVALID_ARGUMENT, "Unknown terminal identifier"};

    const auto transport = transports.find(terminalId);
    if (transport == transports.end())
        return {false, transport::TransportError::NOT_OPEN, "Terminal transport is not configured"};

    model->second.connectionStatus = TerminalConnectionStatus::CONNECTING;
    const transport::TransportResult result = transport->second->open();

    model->second.connectionStatus = result.success
        ? TerminalConnectionStatus::CONNECTED
        : TerminalConnectionStatus::FAILED;

    return result;
}

void TerminalManager::disconnect(const std::string& terminalId)
{
    std::lock_guard<std::mutex> lock(mutex);

    const auto transport = transports.find(terminalId);
    if (transport != transports.end())
        transport->second->close();

    const auto model = terminalModels.find(terminalId);
    if (model != terminalModels.end())
        model->second.connectionStatus = TerminalConnectionStatus::DISCONNECTED;
}

transport::TransportBytes TerminalManager::sendAndReceive(
    const std::string& terminalId,
    const std::vector<std::uint8_t>& request,
    std::size_t maximumResponseBytes)
{
    std::lock_guard<std::mutex> lock(mutex);

    const auto transport = transports.find(terminalId);
    if (transport == transports.end())
        return {{false, transport::TransportError::NOT_OPEN, "Terminal transport is not configured"}, {}};

    if (!transport->second->isOpen())
        return {{false, transport::TransportError::NOT_OPEN, "Terminal transport is not open"}, {}};

    const transport::TransportResult sendResult = transport->second->send(request);
    if (!sendResult.success)
    {
        const auto model = terminalModels.find(terminalId);
        if (model != terminalModels.end())
            model->second.connectionStatus = TerminalConnectionStatus::DISCONNECTED;

        return {{sendResult.success, sendResult.error, sendResult.message}, {}};
    }

    const transport::TransportBytes response =
        transport->second->receive(maximumResponseBytes);

    if (!response.result.success)
    {
        const auto model = terminalModels.find(terminalId);
        if (model != terminalModels.end() &&
            response.result.error == transport::TransportError::DISCONNECTED)
            model->second.connectionStatus = TerminalConnectionStatus::DISCONNECTED;
    }

    return response;
}

} // namespace paybridge::terminal
