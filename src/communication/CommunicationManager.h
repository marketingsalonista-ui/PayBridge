#pragma once

#include "../core/TransactionEngine.h"
#include "../security/AuthManager.h"
#include "../core/PaymentOrchestrationService.h"
#include <nlohmann/json.hpp>
#include <atomic>

class CommunicationManager
{
public:
    explicit CommunicationManager(TransactionEngine& engine);
    CommunicationManager(TransactionEngine& engine, const AuthManager& authManager);
    int runServer(unsigned short port,
        const std::atomic_bool* shutdownRequested = nullptr,
        std::atomic_ushort* boundPort = nullptr);

    nlohmann::json handleRequest(const std::string& request) const;
    const paybridge::terminal::TerminalManager& terminalManagerReadOnly() const noexcept;
    PaymentOrchestrationService& paymentService() noexcept;

private:
    TransactionEngine& engine;
    AuthManager authManager;
    paybridge::terminal::TerminalManager terminalManager;
    mutable PaymentOrchestrationService orchestrationService;
};
