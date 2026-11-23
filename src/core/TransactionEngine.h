#pragma once

#include <cstdint>
#include <string>

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
    std::int64_t amountMinor = 0;
    std::string currency;
    TransactionStatus status = TransactionStatus::INVALID;
};

class TransactionEngine
{
public:
    Transaction createTransaction(
        std::int64_t amountMinor,
        const std::string& currency
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
};
