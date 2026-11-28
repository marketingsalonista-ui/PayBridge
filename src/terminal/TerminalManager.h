#pragma once

#include "TerminalModel.h"
#include "ITerminalProtocol.h"
#include "ITerminalAdapter.h"
#include "../transports/ITransport.h"

#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace paybridge::terminal
{

struct AdapterReadModel
{
    std::string adapterId;
    AdapterMetadata metadata;
};

struct TerminalReadModel
{
    std::string terminalId;
    bool simulated = false;
    // True only when a registered terminal also has a transport configured.
    bool configured = false;
    std::string connectionStatus;
    // A configured simulator in DISCONNECTED state can still be connected by
    // the payment workflow when an operation begins.
    bool available = false;
};

class TerminalManager
{
public:
    bool registerTerminal(const TerminalModel& terminal);
    bool removeTerminal(const std::string& terminalId);
    bool containsTerminal(const std::string& terminalId) const;
    std::vector<TerminalModel> terminals() const;
    std::vector<TerminalReadModel> terminalSnapshots() const;
    std::vector<AdapterReadModel> adapterSnapshots() const;
    bool selectSimulatedTerminal(
        const std::string& requestedTerminalId,
        std::string& selectedTerminalId,
        std::string& error) const;
    bool registerAdapter(std::shared_ptr<ITerminalAdapter> adapter);
    bool selectTerminal(const std::string& requestedTerminalId,
        std::string& selectedTerminalId, std::string& error) const;
    // Receives the ID of the adapter selected (and preflight-checked) for this
    // operation, so the caller can persist it together with the SENT state.
    using AdapterAwareBeforeSend = std::function<bool(const std::string& adapterId)>;

    TerminalOperationResult executeOperation(
        const TerminalOperationRequest& request,
        const AdapterAwareBeforeSend& beforeSend);

    TerminalOperationResult executeOperation(
        const TerminalOperationRequest& request,
        const ITerminalAdapter::BeforeSend& beforeSend);

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
    std::unordered_map<std::string, std::shared_ptr<ITerminalAdapter>> adapters;
    mutable std::unordered_map<std::string, std::shared_ptr<std::mutex>> operationLocks;

    std::shared_ptr<std::mutex> operationLockFor(const std::string& terminalId) const;
};

} // namespace paybridge::terminal
