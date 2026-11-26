#pragma once

#include "TransactionEngine.h"

#include <string>
#include <vector>
#include <cstddef>

class TransactionLogger
{
public:
    struct CsvRecord
    {
        std::size_t rowNumber = 0;
        std::size_t columnCount = 0;
        std::string rawRecord;
        Transaction transaction;
        bool valid = false;
        std::string diagnostic;
    };

    struct CsvReadReport
    {
        bool sourceExists = false;
        bool headerValid = false;
        std::string rawHeader;
        std::vector<CsvRecord> records;
        std::string diagnostic;
    };

    // Writes a transaction to the CSV log.
    static bool logTransaction(const Transaction& transaction);

    // Loads transactions from the CSV log.
    static std::vector<Transaction> loadTransactions();

    // Reads one CSV source without changing the configured logger path and
    // returns every logical record, including malformed records.
    static CsvReadReport readTransactionsFromFile(const std::string& path);

    static std::string filePath();

    // Allows tests to use a separate transaction log.
    static void setFilePath(const std::string& path);

    // Restores the default production log path.
    static void resetFilePath();
};
