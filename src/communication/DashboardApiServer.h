#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

class AuthManager;
class TransactionEngine;
namespace paybridge::terminal { class TerminalManager; }
namespace paybridge::assets { class AssetRegistry; }
class PaymentOrchestrationService;

class DashboardApiServer
{
public:
    struct Config
    {
        std::uint16_t port = 5051;
        std::size_t workerCount = 2;
        std::size_t maximumConnections = 16;
    };

    DashboardApiServer();
    explicit DashboardApiServer(Config config);
    DashboardApiServer(Config config, const AuthManager& authManager,
        TransactionEngine& transactionEngine,
        const paybridge::terminal::TerminalManager& terminalManager,
        const paybridge::assets::AssetRegistry& assetRegistry);
    DashboardApiServer(Config config, const AuthManager& authManager,
        TransactionEngine& transactionEngine,
        const paybridge::terminal::TerminalManager& terminalManager,
        const paybridge::assets::AssetRegistry& assetRegistry,
        PaymentOrchestrationService& paymentService);
    ~DashboardApiServer();

    DashboardApiServer(const DashboardApiServer&) = delete;
    DashboardApiServer& operator=(const DashboardApiServer&) = delete;

    bool start();
    void stop() noexcept;
    void join() noexcept;
    bool isRunning() const noexcept;
    std::uint16_t boundPort() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl;
};
