#pragma once

#include "../assets/Asset.h"
#include "../assets/AssetRegistry.h"
#include "../money/Money.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <mutex>

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

    // Complete asset identity and exact-money representation.
    paybridge::assets::Asset asset;
    paybridge::money::Money money;
};

class TransactionEngine
{
public:
    TransactionEngine();

    // Backward-compatible fiat API.
    Transaction createTransaction(
        std::int64_t amountMinor,
        const std::string& currency,
        const std::string& externalReference
    );

    // Asset-aware exact-money API.
    Transaction createTransaction(
        const paybridge::money::Money& money,
        const paybridge::assets::Asset& asset,
        const std::string& externalReference
    );

    bool updateStatus(
        Transaction& transaction,
        TransactionStatus newStatus
    );

    static std::string statusToString(TransactionStatus status);

    paybridge::assets::AssetRegistry& assets();

private:
    bool validateTransaction(
        std::int64_t amountMinor,
        const std::string& currency
    );

    static bool isValidTransition(
        TransactionStatus currentStatus,
        TransactionStatus newStatus
    );

    // Protects shared transaction state from concurrent access.
    std::mutex engineMutex;

    std::unordered_map<std::string, Transaction> transactionsByReference;

    void restoreTransactions();

    paybridge::assets::AssetRegistry assetRegistry;
};
