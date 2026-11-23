#pragma once

#include "../core/TransactionEngine.h"
#include <string>

class TransactionLogger
{
public:
    static bool logTransaction(const Transaction& transaction);
};
