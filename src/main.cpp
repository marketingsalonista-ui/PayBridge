#include <iostream>
#include <cstdlib>
#include <atomic>
#include <windows.h>

#include "communication/CommunicationManager.h"
#include "communication/DashboardApiServer.h"
#include "core/TransactionLogger.h"
#include "core/TransactionRepository.h"

namespace
{
std::atomic_bool* shutdownSignal = nullptr;

BOOL WINAPI handleConsoleShutdown(DWORD event)
{
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT || event == CTRL_CLOSE_EVENT)
    {
        if (shutdownSignal)
            shutdownSignal->store(true);
        return TRUE;
    }
    return FALSE;
}
}

int main()
{
    std::cout << "\n====================================\n";
    std::cout << "          PAYBRIDGE SYSTEM          \n";
    std::cout << "====================================\n";

    const char* configuredDatabasePath = std::getenv("PAYBRIDGE_DATABASE_PATH");
    const std::string databasePath = configuredDatabasePath && *configuredDatabasePath
        ? configuredDatabasePath
        : TransactionRepository::defaultDatabasePath();
    TransactionEngine engine(databasePath, TransactionLogger::filePath());
    AuthManager authManager = AuthManager::fromEnvironment();
    CommunicationManager communication(engine, authManager);

    const char* usdtNetwork = std::getenv("PAYBRIDGE_USDT_NETWORK");
    const char* usdtContract = std::getenv("PAYBRIDGE_USDT_CONTRACT_ADDRESS");
    const char* usdtPrecision = std::getenv("PAYBRIDGE_USDT_PRECISION");

    if (usdtNetwork && usdtContract && usdtPrecision)
    {
        try
        {
            const unsigned int precision = std::stoul(usdtPrecision);
            if (precision <= 18)
            {
                engine.assets().registerUsdt(
                    usdtNetwork,
                    usdtContract,
                    static_cast<std::uint8_t>(precision)
                );
            }
        }
        catch (...) {}
    }

    if (!engine.initialize())
    {
        std::cerr << "[ERROR] Transaction persistence initialization failed: "
                  << engine.persistenceError() << std::endl;
        return 1;
    }

    std::cout << "[READY] Communication Manager\n";

    DashboardApiServer dashboardApi({}, authManager, engine,
        communication.terminalManagerReadOnly(), engine.assets(),
        communication.paymentService());
    if (!dashboardApi.start())
    {
        std::cerr << "[ERROR] Dashboard HTTP server failed to start on 127.0.0.1:5051\n";
        return 1;
    }
    std::cout << "[READY] Dashboard API listening on 127.0.0.1:"
              << dashboardApi.boundPort() << "\n";

    std::atomic_bool shutdownRequested{false};
    shutdownSignal = &shutdownRequested;
    const bool handlerRegistered = SetConsoleCtrlHandler(handleConsoleShutdown, TRUE) != FALSE;
    const int result = communication.runServer(5050, &shutdownRequested);
    shutdownRequested.store(true);
    if (handlerRegistered)
        SetConsoleCtrlHandler(handleConsoleShutdown, FALSE);
    shutdownSignal = nullptr;
    dashboardApi.stop();
    dashboardApi.join();
    std::cout << "[STOPPED] PayBridge servers shut down cleanly\n";
    return result;
}
