#include "TransactionLogger.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <chrono>
#include <ctime>
#include <iomanip>

bool TransactionLogger::logTransaction(
    const Transaction& transaction)
{
    try
    {
        std::filesystem::create_directories("logs");

        const std::string filename = "logs/transactions.csv";

        bool needsHeader = !std::filesystem::exists(filename)
            || std::filesystem::file_size(filename) == 0;

        std::ofstream file(filename, std::ios::app);

        if (!file.is_open())
        {
            std::cerr << "[ERROR] Could not open transaction log.\n";
            return false;
        }

        if (needsHeader)
        {
            file << "Transaction ID,Amount,Currency,Status,Timestamp,Environment\n";
        }

        auto now = std::chrono::system_clock::now();
        std::time_t currentTime =
            std::chrono::system_clock::to_time_t(now);

        std::tm localTime{};

#ifdef _WIN32
        localtime_s(&localTime, &currentTime);
#else
        localtime_r(&currentTime, &localTime);
#endif

        double amount =
            static_cast<double>(transaction.amountMinor) / 100.0;

        file << transaction.transactionId << ","
             << std::fixed << std::setprecision(2) << amount << ","
             << transaction.currency << ","
             << TransactionEngine::statusToString(transaction.status) << ","
             << std::put_time(&localTime, "%Y-%m-%d %H:%M:%S") << ","
             << "SIMULATION\n";

        file.close();

        std::cout << "[LOGGED] Transaction saved to "
                  << filename << "\n";

        return true;
    }
    catch (const std::exception& error)
    {
        std::cerr << "[ERROR] Logging failed: "
                  << error.what() << "\n";

        return false;
    }
}
