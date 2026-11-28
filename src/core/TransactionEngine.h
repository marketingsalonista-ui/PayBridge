#pragma once

#include "../assets/Asset.h"
#include "../assets/AssetRegistry.h"
#include "../money/Money.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <mutex>
#include <optional>
#include <vector>

class TransactionRepository;

enum class TransactionStatus
{
    CREATED,
    SENT,
    RECEIVED,
    APPROVED,
    DECLINED,
    FAILED,
    UNKNOWN,
    INVALID
};

// Dashboard-safe projection, intentionally excluding external references and
// replay/transport internals while preserving exact integer money units.
struct TransactionReadRecord
{
    std::string transactionId;
    paybridge::money::Money money;
    paybridge::assets::Asset asset;
    TransactionStatus status = TransactionStatus::INVALID;
    std::int64_t createdAtUnix = 0;
    std::int64_t updatedAtUnix = 0;
    std::string terminalId;
    std::string adapterId;
};

struct TransactionCursor
{
    std::int64_t createdAtUnix = 0;
    std::string transactionId;
};

struct TransactionPage
{
    bool success = false;
    bool validRequest = true;
    bool hasMore = false;
    std::vector<TransactionReadRecord> transactions;
    std::optional<TransactionCursor> nextCursor;
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

    // Adapter that handled this transaction. Empty means "do not change the stored value".
    std::string adapterId;
};

enum class ReplayState
{
    RESERVED,
    PROCESSING,
    COMPLETED,
    FAILED,
    UNKNOWN
};

struct RequestReplayData
{
    std::string requestId;
    std::string canonicalRequestHash;
    std::string paymentFingerprint;
    std::int64_t requestTimestamp = 0;
    std::int64_t requestExpiry = 0;
};

enum class ReplayRegistrationCode
{
    NEW_REQUEST,
    IDENTICAL_RETRY,
    EXISTING_EXTERNAL_REFERENCE,
    REQUEST_ID_CONFLICT,
    EXTERNAL_REFERENCE_CONFLICT,
    DATABASE_ERROR
};

struct ReplayAwareTransactionResult
{
    ReplayRegistrationCode code = ReplayRegistrationCode::DATABASE_ERROR;
    Transaction transaction;
    ReplayState replayState = ReplayState::UNKNOWN;
    std::string responseStatus;
    std::string adapterResultCode;
    std::string adapterMessage;
    std::string unknownReason;
    std::string terminalId;
    std::string simulationContract;
    bool shouldProcess = false;
};

class TransactionEngine
{
public:
    TransactionEngine();
    TransactionEngine(std::string databasePath, std::string legacyCsvPath);
    ~TransactionEngine();

    TransactionEngine(const TransactionEngine&) = delete;
    TransactionEngine& operator=(const TransactionEngine&) = delete;

    // Initializes SQLite, imports the legacy CSV once when configured, and
    // restores the in-memory index. Call after registering configured assets.
    bool initialize();
    bool isReady() const noexcept;
    std::string persistenceError() const;
    TransactionPage listTransactions(
        std::size_t pageSize = 50,
        std::optional<TransactionStatus> statusFilter = std::nullopt,
        std::optional<TransactionCursor> cursor = std::nullopt) const;
    std::optional<TransactionReadRecord> findTransactionById(
        const std::string& transactionId) const;

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

    ReplayAwareTransactionResult createTransaction(
        const paybridge::money::Money& money,
        const paybridge::assets::Asset& asset,
        const std::string& externalReference,
        const RequestReplayData& replayData);
    bool bindSimulatedTerminal(const std::string& requestId,
        const std::string& terminalId, const std::string& contractVersion);

    bool updateStatus(
        Transaction& transaction,
        TransactionStatus newStatus
    );

    bool updateStatusForRequest(
        Transaction& transaction,
        TransactionStatus newStatus,
        const std::string& requestId,
        ReplayState replayState,
        const std::string& responseStatus,
        const std::string& adapterResultCode = {},
        const std::string& adapterMessage = {},
        const std::string& unknownReason = {});

    static std::string statusToString(TransactionStatus status);

    static bool isValidTransition(
        TransactionStatus currentStatus,
        TransactionStatus newStatus
    );

    paybridge::assets::AssetRegistry& assets();

private:
    bool validateTransaction(
        std::int64_t amountMinor,
        const std::string& currency
    );



    // Protects shared transaction state from concurrent access.
    mutable std::mutex engineMutex;

    std::unordered_map<std::string, Transaction> transactionsByReference;

    bool restoreTransactions();

    std::unique_ptr<TransactionRepository> repository;
    std::string legacyCsvPath;
    bool ready = false;
    std::string persistenceErrorMessage;

    paybridge::assets::AssetRegistry assetRegistry;
};
