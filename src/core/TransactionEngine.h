
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

enum class TransactionStatus
{
    CREATED,
    SENT,
    RECEIVED,
    APPROVED,
    DECLINED,
    FAILED,
    INVALID
};

struct Transaction
{
    std::string transactionId;
    std::string externalReference;
    std::int64_t amountMinor = 0;
    std::string currency;
    TransactionStatus status = TransactionStatus::INVALID;
};

class TransactionEngine
{
public:
    TransactionEngine();

    Transaction createTransaction(
        std::int64_t amountMinor,
        const std::string& currency,
        const std::string& externalReference
    );

    bool updateStatus(
        Transaction& transaction,
        TransactionStatus newStatus
    );

    static std::string statusToString(TransactionStatus status);

private:
    bool validateTransaction(
        std::int64_t amountMinor,
        const std::string& currency
    );

    static bool isValidTransition(
        TransactionStatus currentStatus,
        TransactionStatus newStatus
    );

    std::unordered_map<std::string, Transaction> transactionsByReference;

    void restoreTransactions();
};
