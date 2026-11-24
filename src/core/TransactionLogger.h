#pragma once

#include "TransactionEngine.h"
#include <string>
#include <vector>

class TransactionLogger
{
public:
    static bool logTransaction(const Transaction& transaction);

    static std::vector<Transaction> loadTransactions();

    // Allows tests to use a separate transaction log.
    static void setFilePath(const std::string& path);

    // Restores the default production log path.
    static void resetFilePath();
};