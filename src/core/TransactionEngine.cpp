
#include "TransactionEngine.h"
#include "TransactionLogger.h"

#include <iomanip>
#include <sstream>
#include <objbase.h>

TransactionEngine::TransactionEngine()
{
    restoreTransactions();
}

void TransactionEngine::restoreTransactions()
{
    const auto savedTransactions =
        TransactionLogger::loadTransactions();

    for (const auto& transaction : savedTransactions)
    {
        transactionsByReference[transaction.externalReference] =
            transaction;
    }
}

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
    const std::string& currency,
    const std::string& externalReference)
{
    Transaction transaction;

    transaction.amountMinor = amountMinor;
    transaction.currency = currency;
    transaction.externalReference = externalReference;

    if (!validateTransaction(amountMinor, currency) ||
        externalReference.empty())
    {
        transaction.status = TransactionStatus::INVALID;
        return transaction;
    }

    auto existing = transactionsByReference.find(externalReference);

    if (existing != transactionsByReference.end())
    {
        const Transaction& previous = existing->second;

        if (previous.amountMinor != amountMinor ||
            previous.currency != currency)
        {
            transaction.status = TransactionStatus::INVALID;
            return transaction;
        }

        return previous;
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

    transactionsByReference.emplace(
        externalReference,
        transaction
    );

    TransactionLogger::logTransaction(transaction);

    return transaction;
}

bool TransactionEngine::updateStatus(
    Transaction& transaction,
    TransactionStatus newStatus)
{
    if (transaction.transactionId.empty())
        return false;

    if (!isValidTransition(transaction.status, newStatus))
        return false;

    transaction.status = newStatus;

    auto existing = transactionsByReference.find(
        transaction.externalReference
    );

    if (existing != transactionsByReference.end())
    {
        existing->second = transaction;
    }

    TransactionLogger::logTransaction(transaction);

    return true;
}

bool TransactionEngine::isValidTransition(
    TransactionStatus currentStatus,
    TransactionStatus newStatus)
{
    switch (currentStatus)
    {
        case TransactionStatus::CREATED:
            return newStatus == TransactionStatus::SENT ||
                   newStatus == TransactionStatus::FAILED;

        case TransactionStatus::SENT:
            return newStatus == TransactionStatus::RECEIVED ||
                   newStatus == TransactionStatus::FAILED;

        case TransactionStatus::RECEIVED:
            return newStatus == TransactionStatus::APPROVED ||
                   newStatus == TransactionStatus::DECLINED ||
                   newStatus == TransactionStatus::FAILED;

        case TransactionStatus::APPROVED:
        case TransactionStatus::DECLINED:
        case TransactionStatus::FAILED:
        case TransactionStatus::INVALID:
            return false;

        default:
            return false;
    }
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
