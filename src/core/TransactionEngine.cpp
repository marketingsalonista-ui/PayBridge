#include "TransactionEngine.h"

#include <iomanip>
#include <sstream>
#include <objbase.h>

bool TransactionEngine::validateTransaction(
    std::int64_t amountMinor,
    const std::string& currency)
{
    if (amountMinor <= 0)
        return false;

    if (currency.length() != 3)
        return false;

    for (char c : currency)
    {
        if (c < 'A' || c > 'Z')
            return false;
    }

    return true;
}

Transaction TransactionEngine::createTransaction(
    std::int64_t amountMinor,
    const std::string& currency)
{
    Transaction transaction;

    transaction.amountMinor = amountMinor;
    transaction.currency = currency;

    if (!validateTransaction(amountMinor, currency))
    {
        transaction.status = TransactionStatus::INVALID;
        return transaction;
    }

    GUID guid{};

    if (CoCreateGuid(&guid) != S_OK)
    {
        transaction.status = TransactionStatus::FAILED;
        return transaction;
    }

    std::ostringstream id;

    id << "PB-"
       << std::uppercase << std::hex
       << std::setfill('0')
       << std::setw(8) << guid.Data1
       << std::setw(4) << guid.Data2
       << std::setw(4) << guid.Data3;

    for (int i = 0; i < 8; ++i)
    {
        id << std::setw(2)
           << static_cast<unsigned int>(guid.Data4[i]);
    }

    transaction.transactionId = id.str();
    transaction.status = TransactionStatus::CREATED;

    return transaction;
}

bool TransactionEngine::updateStatus(
    Transaction& transaction,
    TransactionStatus newStatus)
{
    if (transaction.transactionId.empty())
        return false;

    transaction.status = newStatus;
    return true;
}

std::string TransactionEngine::statusToString(
    TransactionStatus status)
{
    switch (status)
    {
        case TransactionStatus::CREATED: return "CREATED";
        case TransactionStatus::SENT: return "SENT";
        case TransactionStatus::RECEIVED: return "RECEIVED";
        case TransactionStatus::APPROVED: return "APPROVED";
        case TransactionStatus::DECLINED: return "DECLINED";
        case TransactionStatus::FAILED: return "FAILED";
        case TransactionStatus::INVALID: return "INVALID";
        default: return "UNKNOWN";
    }
}
