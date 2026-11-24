
#include "TransactionLogger.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace
{
    const std::string defaultFilename = "logs/transactions.csv";
    std::string currentFilename = defaultFilename;

    std::string getTimestamp()
    {
        auto now = std::chrono::system_clock::now();
        std::time_t currentTime =
            std::chrono::system_clock::to_time_t(now);

        std::tm localTime{};

#ifdef _WIN32
        localtime_s(&localTime, &currentTime);
#else
        localtime_r(&currentTime, &localTime);
#endif

        std::ostringstream timestamp;
        timestamp << std::put_time(&localTime, "%Y-%m-%d %H:%M:%S");

        return timestamp.str();
    }

    TransactionStatus stringToStatus(const std::string& status)
    {
        if (status == "CREATED") return TransactionStatus::CREATED;
        if (status == "SENT") return TransactionStatus::SENT;
        if (status == "RECEIVED") return TransactionStatus::RECEIVED;
        if (status == "APPROVED") return TransactionStatus::APPROVED;
        if (status == "DECLINED") return TransactionStatus::DECLINED;
        if (status == "FAILED") return TransactionStatus::FAILED;

        return TransactionStatus::INVALID;
    }
}

void TransactionLogger::setFilePath(const std::string& path)
{
    if (!path.empty())
        currentFilename = path;
}

void TransactionLogger::resetFilePath()
{
    currentFilename = defaultFilename;
}

bool TransactionLogger::logTransaction(
    const Transaction& transaction)
{
    try
    {
        std::filesystem::path logPath(currentFilename);
        std::filesystem::path parent = logPath.parent_path();

        if (!parent.empty())
            std::filesystem::create_directories(parent);

        bool needsHeader =
            !std::filesystem::exists(currentFilename) ||
            std::filesystem::file_size(currentFilename) == 0;

        std::ofstream file(currentFilename, std::ios::app);

        if (!file.is_open())
        {
            std::cerr << "[ERROR] Could not open transaction log.\n";
            return false;
        }

        if (needsHeader)
        {
            file << "TransactionID,ExternalReference,AmountMinor,Currency,Status,Timestamp,Environment\n";
        }

        file << transaction.transactionId << ","
             << transaction.externalReference << ","
             << transaction.amountMinor << ","
             << transaction.currency << ","
             << TransactionEngine::statusToString(transaction.status) << ","
             << getTimestamp() << ","
             << "SIMULATION\n";

        file.close();

        if (!file)
        {
            std::cerr << "[ERROR] Failed writing transaction log.\n";
            return false;
        }

        std::cout << "[LOGGED] Transaction saved to "
                  << currentFilename << "\n";

        return true;
    }
    catch (const std::exception& error)
    {
        std::cerr << "[ERROR] Logging failed: "
                  << error.what() << "\n";

        return false;
    }
}

std::vector<Transaction> TransactionLogger::loadTransactions()
{
    std::vector<Transaction> transactions;

    try
    {
        if (!std::filesystem::exists(currentFilename))
            return transactions;

        std::ifstream file(currentFilename);

        if (!file.is_open())
        {
            std::cerr << "[ERROR] Could not read transaction log.\n";
            return transactions;
        }

        std::string line;

        // Skip CSV header.
        std::getline(file, line);

        while (std::getline(file, line))
        {
            std::stringstream row(line);

            Transaction transaction;
            std::string amount;
            std::string status;
            std::string timestamp;
            std::string environment;

            if (!std::getline(row, transaction.transactionId, ',') ||
                !std::getline(row, transaction.externalReference, ',') ||
                !std::getline(row, amount, ',') ||
                !std::getline(row, transaction.currency, ',') ||
                !std::getline(row, status, ',') ||
                !std::getline(row, timestamp, ',') ||
                !std::getline(row, environment))
            {
                continue;
            }

            try
            {
                std::size_t parsed = 0;

                transaction.amountMinor = std::stoll(amount, &parsed);

                if (parsed != amount.size() ||
                    transaction.transactionId.empty() ||
                    transaction.externalReference.empty() ||
                    transaction.currency.size() != 3)
                {
                    continue;
                }
            }
            catch (...)
            {
                continue;
            }

            transaction.status = stringToStatus(status);

            if (transaction.status == TransactionStatus::INVALID)
                continue;

            transactions.push_back(transaction);
        }
    }
    catch (const std::exception& error)
    {
        std::cerr << "[ERROR] Loading transactions failed: "
                  << error.what() << "\n";
    }

    return transactions;
}
