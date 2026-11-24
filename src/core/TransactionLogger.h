#pragma once

#include "TransactionEngine.h"

#include <string>
#include <vector>

class TransactionLogger
{
public:
    // Writes a transaction to the CSV log.
    static bool logTransaction(const Transaction& transaction);

    // Loads transactions from the CSV log.
    static std::vector<Transaction> loadTransactions();

    // Allows tests to use a separate transaction log.
    static void setFilePath(const std::string& path);

    // Restores the default production log path.
    static void resetFilePath();
};