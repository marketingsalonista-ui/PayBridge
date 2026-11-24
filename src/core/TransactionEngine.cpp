#include "TransactionEngine.h"
#include "TransactionLogger.h"

#include "../money/AmountParser.h"

#include <iomanip>
#include <sstream>
#include <objbase.h>

TransactionEngine::TransactionEngine()
{
    restoreTransactions();
}

paybridge::assets::AssetRegistry& TransactionEngine::assets()
{
    return assetRegistry;
}

void TransactionEngine::restoreTransactions()
{
    const auto savedTransactions =
        TransactionLogger::loadTransactions();

    for (Transaction transaction : savedTransactions)
    {
        if (transaction.asset.isComplete() &&
            assetRegistry.contains(transaction.asset))
        {
            transactionsByReference[transaction.externalReference] = transaction;
            continue;
        }

        const auto fiat = assetRegistry.findFiat(transaction.currency);
        if (fiat)
        {
            transaction.asset = *fiat;
            transaction.money.units = transaction.amountMinor;
            transaction.money.precision = fiat->decimalPrecision;

            transactionsByReference[transaction.externalReference] = transaction;
        }
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
    const auto fiat = assetRegistry.findFiat(currency);
    if (!fiat)
        return Transaction{.externalReference = externalReference,
                           .amountMinor = amountMinor,
                           .currency = currency,
                           .status = TransactionStatus::INVALID};

    paybridge::money::Money money;
    money.units = amountMinor;
    money.precision = fiat->decimalPrecision;

    return createTransaction(money, *fiat, externalReference);
}

Transaction TransactionEngine::createTransaction(
    const paybridge::money::Money& money,
    const paybridge::assets::Asset& asset,
    const std::string& externalReference)
{
    std::lock_guard<std::mutex> lock(engineMutex);

    Transaction transaction;
    transaction.amountMinor = money.units;
    transaction.currency = asset.isFiat() ? asset.code : std::string{};
    transaction.externalReference = externalReference;
    transaction.asset = asset;
    transaction.money = money;

    if (!money.isPositive() ||
        externalReference.empty() ||
        !asset.isComplete() ||
        !assetRegistry.contains(asset) ||
        money.precision != asset.decimalPrecision)
    {
        transaction.status = TransactionStatus::INVALID;
        return transaction;
    }

    auto existing = transactionsByReference.find(externalReference);

    if (existing != transactionsByReference.end())
    {
        const Transaction& previous = existing->second;

        if (previous.money.units != money.units ||
            previous.asset != asset)
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

    if (!TransactionLogger::logTransaction(transaction))
    {
        transaction.status = TransactionStatus::FAILED;
        return transaction;
    }

    transactionsByReference.emplace(externalReference, transaction);
    return transaction;
}

bool TransactionEngine::updateStatus(
    Transaction& transaction,
    TransactionStatus newStatus)
{
    std::lock_guard<std::mutex> lock(engineMutex);

    if (transaction.transactionId.empty() ||
        transaction.externalReference.empty())
    {
        return false;
    }

    // Find the authoritative transaction in engine memory.
    auto existing = transactionsByReference.find(
        transaction.externalReference
    );

    if (existing == transactionsByReference.end())
        return false;

    const Transaction& storedTransaction = existing->second;

    // Reject modified or mismatched transaction details.
    if (transaction.transactionId != storedTransaction.transactionId ||
        transaction.amountMinor != storedTransaction.amountMinor ||
        transaction.currency != storedTransaction.currency ||
        transaction.money.units != storedTransaction.money.units ||
        transaction.money.precision != storedTransaction.money.precision ||
        transaction.asset != storedTransaction.asset ||
        transaction.status != storedTransaction.status)
    {
        return false;
    }

    // Validate transition against the stored status.
    if (!isValidTransition(storedTransaction.status, newStatus))
        return false;

    Transaction updatedTransaction = storedTransaction;
    updatedTransaction.status = newStatus;

    // Persist before changing the in-memory record.
    if (!TransactionLogger::logTransaction(updatedTransaction))
        return false;

    existing->second = updatedTransaction;

    // Synchronize the caller's transaction object.
    transaction = updatedTransaction;

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
