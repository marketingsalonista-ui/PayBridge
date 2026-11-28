#include "TerminalManager.h"

#include <algorithm>
#include <atomic>
#include <exception>

namespace paybridge::terminal
{
namespace
{
class GuardedTransport final : public transport::ITransport
{
public:
    GuardedTransport(transport::ITransport& underlying,
        const std::atomic<bool>& sendAuthorized)
        : underlying(underlying), sendAuthorized(sendAuthorized) {}

    transport::TransportResult open() override { return underlying.open(); }
    void close() noexcept override { underlying.close(); }
    bool isOpen() const noexcept override { return underlying.isOpen(); }
    bool isSimulationTransport() const noexcept override
    {
        return underlying.isSimulationTransport();
    }

    transport::TransportResult send(
        const std::vector<std::uint8_t>& bytes) override
    {
        if (!sendAuthorized.load())
        {
            return {
                false,
                transport::TransportError::INVALID_ARGUMENT,
                "Send blocked: pre-send persistence has not succeeded"
            };
        }
        return underlying.send(bytes);
    }

    transport::TransportBytes receive(std::size_t maximumBytes) override
    {
        return underlying.receive(maximumBytes);
    }

    void setReceiveTimeoutMilliseconds(std::uint32_t milliseconds) noexcept override
    {
        underlying.setReceiveTimeoutMilliseconds(milliseconds);
    }

    void setSendTimeoutMilliseconds(std::uint32_t milliseconds) noexcept override
    {
        underlying.setSendTimeoutMilliseconds(milliseconds);
    }

    std::string description() const override { return underlying.description(); }

private:
    transport::ITransport& underlying;
    const std::atomic<bool>& sendAuthorized;
};

bool adapterSupports(const std::shared_ptr<ITerminalAdapter>& adapter,
    const TerminalModel& terminal) noexcept
{
    try { return adapter && adapter->supports(terminal); }
    catch (...) { return false; }
}
}

bool TerminalManager::registerTerminal(const TerminalModel& terminal)
{
    if (terminal.terminalId.empty())
        return false;
    std::shared_ptr<std::mutex> operationLock;
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto& storedLock = operationLocks[terminal.terminalId];
        if (!storedLock) storedLock = std::make_shared<std::mutex>();
        operationLock = storedLock;
    }
    std::lock_guard<std::mutex> operation(*operationLock);
    std::lock_guard<std::mutex> lock(mutex);
    if (!terminal.adapterId.empty())
    {
        const auto adapter = adapters.find(terminal.adapterId);
        if (adapter == adapters.end() || !adapterSupports(adapter->second, terminal))
            return false;
    }
    terminalModels[terminal.terminalId] = terminal;
    return true;
}

std::shared_ptr<std::mutex> TerminalManager::operationLockFor(const std::string& terminalId) const
{
    std::lock_guard<std::mutex> lock(mutex);
    const auto found = operationLocks.find(terminalId);
    return found == operationLocks.end() ? nullptr : found->second;
}

bool TerminalManager::removeTerminal(const std::string& terminalId)
{
    const auto operationLock = operationLockFor(terminalId);
    if (operationLock)
    {
        std::lock_guard<std::mutex> operation(*operationLock);
        std::unique_ptr<transport::ITransport> removedTransport;
        bool removed = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            const auto transport = transports.find(terminalId);
            if (transport != transports.end())
            {
                removedTransport = std::move(transport->second);
                transports.erase(transport);
            }
            removed = terminalModels.erase(terminalId) != 0;
        }
        if (removedTransport) removedTransport->close();
        // Keep this mutex for the lifetime of the manager. A thread may
        // already be waiting on its shared_ptr; erasing it could let a
        // re-registered terminal acquire a second lock and overlap that call.
        return removed;
    }
    std::lock_guard<std::mutex> lock(mutex);
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

std::vector<TerminalReadModel> TerminalManager::terminalSnapshots() const
{
    std::lock_guard<std::mutex> lock(mutex);
    std::vector<TerminalReadModel> result;
    result.reserve(terminalModels.size());
    for (const auto& [id, model] : terminalModels)
    {
        const bool configured = transports.find(id) != transports.end();
        const bool simulated = model.transportKind == TransportKind::SIMULATED;
        const bool connectionReady =
            model.connectionStatus == TerminalConnectionStatus::DISCONNECTED ||
            model.connectionStatus == TerminalConnectionStatus::CONNECTED;
        result.push_back({id, simulated, configured,
            terminalConnectionStatusToString(model.connectionStatus),
            configured && simulated && connectionReady});
    }
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right)
    {
        return left.terminalId < right.terminalId;
    });
    return result;
}

bool TerminalManager::selectSimulatedTerminal(
    const std::string& requestedTerminalId,
    std::string& selectedTerminalId,
    std::string& error) const
{
    std::lock_guard<std::mutex> lock(mutex);
    selectedTerminalId.clear();
    error.clear();
    if (!requestedTerminalId.empty())
    {
        const auto model = terminalModels.find(requestedTerminalId);
        if (model == terminalModels.end() || model->second.transportKind != TransportKind::SIMULATED ||
            transports.find(requestedTerminalId) == transports.end())
        { error = "Requested simulated terminal is not configured"; return false; }
        selectedTerminalId = requestedTerminalId;
        return true;
    }

    for (const auto& [id, model] : terminalModels)
    {
        if (model.transportKind != TransportKind::SIMULATED || transports.find(id) == transports.end())
            continue;
        if (!selectedTerminalId.empty())
        { selectedTerminalId.clear(); error = "Terminal selection is ambiguous"; return false; }
        selectedTerminalId = id;
    }
    if (selectedTerminalId.empty())
    { error = "No configured simulated terminal is available"; return false; }
    return true;
}

bool TerminalManager::registerAdapter(std::shared_ptr<ITerminalAdapter> adapter)
{
    if (!adapter) return false;
    std::string id;
    try { id = adapter->adapterId(); }
    catch (...) { return false; }
    if (id.empty() || id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-")
        != std::string::npos) return false;
    std::lock_guard<std::mutex> lock(mutex);
    if (adapters.find(id) != adapters.end()) return false;
    adapters.emplace(id, std::move(adapter));
    return true;
}

std::vector<AdapterReadModel> TerminalManager::adapterSnapshots() const
{
    std::vector<std::pair<std::string, std::shared_ptr<ITerminalAdapter>>> registered;

    {
        std::lock_guard<std::mutex> lock(mutex);
        registered.reserve(adapters.size());

        for (const auto& [id, adapter] : adapters)
            registered.emplace_back(id, adapter);
    }

    std::vector<AdapterReadModel> result;
    result.reserve(registered.size());

    for (const auto& [id, adapter] : registered)
    {
        AdapterMetadata metadata{
            id,
            "Unknown",
            "Unknown",
            "UNSPECIFIED",
            "UNVERIFIED",
            "Adapter metadata could not be retrieved"
        };

        try
        {
            if (adapter)
                metadata = adapter->metadata();
        }
        catch (...)
        {
            // Retain safe fallback metadata.
        }

        result.push_back({id, std::move(metadata)});
    }

    std::sort(result.begin(), result.end(),
        [](const auto& left, const auto& right)
        {
            return left.adapterId < right.adapterId;
        });

    return result;
}
bool TerminalManager::selectTerminal(const std::string& requestedTerminalId,
    std::string& selectedTerminalId, std::string& error) const
{
    std::lock_guard<std::mutex> lock(mutex);
    selectedTerminalId.clear();
    error.clear();
    auto compatible = [this](const TerminalModel& model)
    {
        if (transports.find(model.terminalId) == transports.end()) return false;
        if (!model.adapterId.empty())
        {
            const auto adapter = adapters.find(model.adapterId);
            return adapter != adapters.end() && adapterSupports(adapter->second, model);
        }
        std::size_t matches = 0;
        for (const auto& [id, adapter] : adapters)
            if (adapterSupports(adapter, model)) ++matches;
        return matches == 1;
    };
    if (!requestedTerminalId.empty())
    {
        const auto terminal = terminalModels.find(requestedTerminalId);
        if (terminal == terminalModels.end() || !compatible(terminal->second))
        { error = "Requested terminal is not configured with a compatible adapter"; return false; }
        selectedTerminalId = requestedTerminalId;
        return true;
    }
    std::vector<std::string> candidates;
    for (const auto& [id, model] : terminalModels)
        if (compatible(model)) candidates.push_back(id);
    std::sort(candidates.begin(), candidates.end());
    if (candidates.empty()) { error = "No configured compatible terminal is available"; return false; }
    if (candidates.size() != 1) { error = "Terminal selection is ambiguous"; return false; }
    selectedTerminalId = candidates.front();
    return true;
}

TerminalOperationResult TerminalManager::executeOperation(
    const TerminalOperationRequest& request,
    const ITerminalAdapter::BeforeSend& beforeSend)
{
    AdapterAwareBeforeSend adapterAware;
    if (beforeSend)
        adapterAware = [&beforeSend](const std::string&) { return beforeSend(); };
    return executeOperation(request, adapterAware);
}

TerminalOperationResult TerminalManager::executeOperation(
    const TerminalOperationRequest& request,
    const AdapterAwareBeforeSend& beforeSend)
{
    std::string selectedAdapterId;
    auto failure = [&request, &selectedAdapterId](TerminalOperationOutcome outcome, const std::string& code,
        const std::string& message, bool mayHaveSent = false)
    {
        return TerminalOperationResult{outcome, request.requestId, request.transactionId,
            request.externalReference, request.terminalId, code, message, false, mayHaveSent, selectedAdapterId};
    };
    if (!request.isValid())
        return failure(TerminalOperationOutcome::FAILED, "INVALID_OPERATION", "Operation request is invalid");
    const auto operationLock = operationLockFor(request.terminalId);
    if (!operationLock)
        return failure(TerminalOperationOutcome::FAILED, "TERMINAL_UNAVAILABLE", "Terminal is not configured");
    std::lock_guard<std::mutex> operation(*operationLock);

    transport::ITransport* byteTransport = nullptr;
    std::shared_ptr<ITerminalAdapter> adapter;
    TerminalModel selectedModel;
    {
        std::lock_guard<std::mutex> lock(mutex);
        const auto model = terminalModels.find(request.terminalId);
        const auto transport = transports.find(request.terminalId);
        if (model == terminalModels.end() || transport == transports.end())
            return failure(TerminalOperationOutcome::FAILED, "TERMINAL_UNAVAILABLE", "Terminal is not configured");
        selectedModel = model->second;
        byteTransport = transport->second.get();
        if (!model->second.adapterId.empty())
        {
            const auto found = adapters.find(model->second.adapterId);
            if (found != adapters.end() && adapterSupports(found->second, model->second))
            {
                adapter = found->second;
                selectedAdapterId = found->first;
            }
        }
        else
        {
            for (const auto& [id, candidate] : adapters)
                if (adapterSupports(candidate, model->second))
                {
                    if (adapter) return failure(TerminalOperationOutcome::FAILED,
                        "ADAPTER_AMBIGUOUS", "Multiple terminal adapters match");
                    adapter = candidate;
                    selectedAdapterId = id;
                }
        }
        if (!adapter)
            return failure(TerminalOperationOutcome::FAILED, "ADAPTER_UNAVAILABLE", "No compatible adapter is registered");
    }

    // Preflight must run before even checking isOpen(). An unverified adapter
    // must not cause any interaction with its configured transport.
    AdapterPreflightResult readiness;
    try
    {
        readiness = adapter->preflight(selectedModel, *byteTransport);
    }
    catch (...)
    {
        return failure(TerminalOperationOutcome::FAILED,
            "ADAPTER_PREFLIGHT_FAILED",
            "Adapter preflight failed; transport was not accessed");
    }

    if (!readiness.ready)
    {
        const std::string code = readiness.resultCode.empty()
            ? "ADAPTER_NOT_READY" : readiness.resultCode;
        const std::string message = readiness.message.empty()
            ? "Adapter is not ready; transport was not accessed" : readiness.message;

        return failure(TerminalOperationOutcome::FAILED, code, message);
    }

    if (!byteTransport->isOpen())
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            const auto model = terminalModels.find(request.terminalId);
            if (model != terminalModels.end()) model->second.connectionStatus = TerminalConnectionStatus::CONNECTING;
        }
        transport::TransportResult opened;
        try
        {
            opened = byteTransport->open();
        }
        catch (...)
        {
            std::lock_guard<std::mutex> lock(mutex);
            const auto model = terminalModels.find(request.terminalId);
            if (model != terminalModels.end())
                model->second.connectionStatus = TerminalConnectionStatus::FAILED;

            return failure(TerminalOperationOutcome::FAILED,
                "TERMINAL_CONNECT_FAILED", "Terminal connection failed");
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            const auto model = terminalModels.find(request.terminalId);
            if (model != terminalModels.end()) model->second.connectionStatus = opened.success
                ? TerminalConnectionStatus::CONNECTED : TerminalConnectionStatus::FAILED;
        }
        if (!opened.success)
            return failure(TerminalOperationOutcome::FAILED, "TERMINAL_CONNECT_FAILED", "Terminal connection failed");
    }
    std::atomic<bool> beforeSendAttempted{false};
    std::atomic<bool> beforeSendPassed{false};
    const ITerminalAdapter::BeforeSend guardedBeforeSend = [&]()
    {
        if (beforeSendAttempted.exchange(true)) return false;
        if (!beforeSend || !beforeSend(selectedAdapterId)) return false;
        beforeSendPassed.store(true);
        return true;
    };

    GuardedTransport guardedTransport(*byteTransport, beforeSendPassed);

    TerminalOperationResult result;
    try { result = adapter->execute(request, guardedTransport, guardedBeforeSend); }
    catch (...)
    {
        return failure(beforeSendPassed.load() ? TerminalOperationOutcome::UNKNOWN : TerminalOperationOutcome::FAILED,
            beforeSendPassed.load() ? "ADAPTER_OUTCOME_UNKNOWN" : "ADAPTER_FAILURE",
            beforeSendPassed.load() ? "Adapter failed after pre-send persistence; outcome is unresolved"
                             : "Adapter failed before terminal operation", beforeSendPassed.load());
    }
    if (!result.correlatesTo(request))
        return failure((result.requestMayHaveBeenSent || beforeSendPassed.load()) ? TerminalOperationOutcome::UNKNOWN
                : TerminalOperationOutcome::FAILED,
            "ADAPTER_RESPONSE_UNCORRELATED", "Adapter result correlation failed",
            result.requestMayHaveBeenSent || beforeSendPassed.load());
    if ((result.outcome == TerminalOperationOutcome::APPROVED ||
     result.outcome == TerminalOperationOutcome::DECLINED) &&
    (!result.requestMayHaveBeenSent || !beforeSendPassed.load()))
        return failure(TerminalOperationOutcome::UNKNOWN,
            "ADAPTER_RESULT_INVALID", "Adapter returned an outcome without a persisted send", true);
    if (result.outcome == TerminalOperationOutcome::FAILED &&
        (result.requestMayHaveBeenSent || beforeSendPassed.load()))
        return failure(TerminalOperationOutcome::UNKNOWN,
            "ADAPTER_FAILURE_AFTER_SEND", "Adapter failure after send is unresolved", true);
    if (result.outcome == TerminalOperationOutcome::UNKNOWN && !beforeSendPassed.load())
    {
        if (result.requestMayHaveBeenSent)
            return failure(TerminalOperationOutcome::UNKNOWN,
                "ADAPTER_OUTCOME_UNKNOWN",
                "Adapter reports possible transmission without pre-send persistence; outcome is unresolved",
                true);

        return failure(TerminalOperationOutcome::FAILED,
            "ADAPTER_FAILED_BEFORE_SEND",
            "Adapter reported uncertainty before a request was sent");
    }
    result.adapterId = selectedAdapterId;
    return result;
}

void TerminalManager::setTransport(
    const std::string& terminalId,
    std::unique_ptr<transport::ITransport> transport)
{
    if (terminalId.empty() || !transport)
        return;

    const auto operationLock = operationLockFor(terminalId);
    if (!operationLock) return;
    std::lock_guard<std::mutex> operation(*operationLock);

    std::unique_ptr<transport::ITransport> replacedTransport;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (terminalModels.find(terminalId) == terminalModels.end()) return;
        const auto existing = transports.find(terminalId);
        if (existing != transports.end())
        {
            replacedTransport = std::move(existing->second);
            existing->second = std::move(transport);
        }
        else transports.emplace(terminalId, std::move(transport));
    }
    if (replacedTransport) replacedTransport->close();
}

transport::TransportResult TerminalManager::connect(const std::string& terminalId)
{
    const auto operationLock = operationLockFor(terminalId);
    if (!operationLock)
        return {false, transport::TransportError::INVALID_ARGUMENT, "Unknown terminal identifier"};
    std::lock_guard<std::mutex> operation(*operationLock);
    transport::ITransport* selectedTransport = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex);
        const auto model = terminalModels.find(terminalId);
        if (model == terminalModels.end())
            return {false, transport::TransportError::INVALID_ARGUMENT, "Unknown terminal identifier"};
        const auto transport = transports.find(terminalId);
        if (transport == transports.end())
            return {false, transport::TransportError::NOT_OPEN, "Terminal transport is not configured"};
        selectedTransport = transport->second.get();
        if (selectedTransport->isOpen())
        {
            model->second.connectionStatus = TerminalConnectionStatus::CONNECTED;
            return {true, transport::TransportError::NONE, "Terminal transport is already connected"};
        }
        model->second.connectionStatus = TerminalConnectionStatus::CONNECTING;
    }
    transport::TransportResult result;
    try
    {
        result = selectedTransport->open();
    }
    catch (...)
    {
        std::lock_guard<std::mutex> lock(mutex);
        const auto model = terminalModels.find(terminalId);
        if (model != terminalModels.end())
            model->second.connectionStatus = TerminalConnectionStatus::FAILED;

        return {false, transport::TransportError::PLATFORM_ERROR,
            "Terminal connection failed unexpectedly"};
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        const auto model = terminalModels.find(terminalId);
        if (model != terminalModels.end()) model->second.connectionStatus = result.success
            ? TerminalConnectionStatus::CONNECTED : TerminalConnectionStatus::FAILED;
    }
    return result;
}

void TerminalManager::disconnect(const std::string& terminalId)
{
    const auto operationLock = operationLockFor(terminalId);
    if (!operationLock) return;
    std::lock_guard<std::mutex> operation(*operationLock);
    transport::ITransport* selectedTransport = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex);
        const auto transport = transports.find(terminalId);
        if (transport != transports.end()) selectedTransport = transport->second.get();
    }
    if (selectedTransport) selectedTransport->close();
    {
        std::lock_guard<std::mutex> lock(mutex);
        const auto model = terminalModels.find(terminalId);
        if (model != terminalModels.end()) model->second.connectionStatus = TerminalConnectionStatus::DISCONNECTED;
    }
}

transport::TransportBytes TerminalManager::sendAndReceive(
    const std::string& terminalId,
    const std::vector<std::uint8_t>& request,
    std::size_t maximumResponseBytes)
{
    const auto operationLock = operationLockFor(terminalId);
    if (!operationLock)
        return {{false, transport::TransportError::INVALID_ARGUMENT, "Unknown terminal identifier"}, {}};
    std::lock_guard<std::mutex> operation(*operationLock);

    std::unique_lock<std::mutex> lock(mutex);

    const auto transport = transports.find(terminalId);
    if (transport == transports.end())
        return {{false, transport::TransportError::NOT_OPEN, "Terminal transport is not configured"}, {}};

    if (!transport->second->isOpen())
        return {{false, transport::TransportError::NOT_OPEN, "Terminal transport is not open"}, {}};

    transport::ITransport* selectedTransport = transport->second.get();
    lock.unlock();
    transport::TransportResult sendResult;
    try
    {
        sendResult = selectedTransport->send(request);
    }
    catch (...)
    {
        lock.lock();
        const auto model = terminalModels.find(terminalId);
        if (model != terminalModels.end())
            model->second.connectionStatus = TerminalConnectionStatus::FAILED;

        return {{false, transport::TransportError::PLATFORM_ERROR,
            "Transport send threw unexpectedly; delivery may be uncertain"}, {}};
    }
    if (!sendResult.success)
    {
        lock.lock();
        const auto model = terminalModels.find(terminalId);
        if (model != terminalModels.end())
            model->second.connectionStatus = TerminalConnectionStatus::DISCONNECTED;

        return {{sendResult.success, sendResult.error, sendResult.message}, {}};
    }

    transport::TransportBytes response;
    try
    {
        response = selectedTransport->receive(maximumResponseBytes);
    }
    catch (...)
    {
        lock.lock();
        const auto model = terminalModels.find(terminalId);
        if (model != terminalModels.end())
            model->second.connectionStatus = TerminalConnectionStatus::FAILED;

        return {{false, transport::TransportError::PLATFORM_ERROR,
            "Transport receive threw unexpectedly; operation outcome is unresolved"}, {}};
    }

    if (!response.result.success)
    {
        lock.lock();
        const auto model = terminalModels.find(terminalId);
        if (model != terminalModels.end() &&
            response.result.error == transport::TransportError::DISCONNECTED)
            model->second.connectionStatus = TerminalConnectionStatus::DISCONNECTED;
    }

    return response;
}

} // namespace paybridge::terminal
