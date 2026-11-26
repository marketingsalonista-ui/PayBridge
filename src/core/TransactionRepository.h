#pragma once

#include "TransactionEngine.h"

#include <sqlite3.h>
#include <string>
#include <vector>
#include <mutex> // Include mutex for thread safety
#include <cstddef>
#include <cstdint>
#include <optional>

enum class TransactionSaveResult
{
    NEW_TRANSACTION,
    EXISTING_IDENTICAL,
    CONFLICT,
    DATABASE_ERROR
};

struct LegacyCsvRecord
{
    std::size_t rowNumber = 0;
    std::string rawRecord;
    std::string rawRecordHash;
    bool valid = false;
    std::string diagnostic;
    Transaction transaction;
};

struct LegacyMigrationResult
{
    bool success = false;
    bool alreadyMigrated = false;
    std::size_t sourceRowCount = 0;
    std::size_t importedRowCount = 0;
    std::size_t reconciledRowCount = 0;
    std::size_t conflictCount = 0;
    std::vector<std::string> diagnostics;
};

class TransactionRepository
{
public:
    explicit TransactionRepository(const std::string& databasePath);
    ~TransactionRepository();

    TransactionRepository(const TransactionRepository&) = delete;
    TransactionRepository& operator=(const TransactionRepository&) = delete;

    bool initialize();
    bool isInitialized() const noexcept;
    bool isReadyForReads() const noexcept;

    TransactionSaveResult saveTransaction(const Transaction& transaction);
    ReplayAwareTransactionResult registerPaymentRequest(
        const Transaction& candidate,
        const RequestReplayData& replayData);
    bool bindSimulatedTerminal(const std::string& requestId,
        const std::string& terminalId, const std::string& contractVersion);
    bool recoverExpiredProcessingRequests();
    bool updateTransactionStatus(const Transaction& transaction);
    bool updateTransactionStatus(
        const Transaction& transaction,
        TransactionStatus expectedStatus);
    bool updateTransactionStatusForRequest(
        const Transaction& transaction,
        TransactionStatus expectedStatus,
        const std::string& requestId,
        ReplayState replayState,
        const std::string& responseStatus,
        const std::string& adapterResultCode,
        const std::string& adapterMessage,
        const std::string& unknownReason);
    std::vector<Transaction> loadTransactions() const;
    TransactionPage listTransactions(
        std::size_t pageSize = 50,
        std::optional<TransactionStatus> statusFilter = std::nullopt,
        std::optional<TransactionCursor> cursor = std::nullopt) const;
    std::optional<TransactionReadRecord> findTransactionById(
        const std::string& transactionId) const;
    bool findByExternalReference(
        const std::string& externalReference,
        Transaction& transaction) const;
    LegacyMigrationResult importLegacyCsv(
        const std::string& sourceName,
        const std::string& sourceHash,
        const std::vector<LegacyCsvRecord>& records);

    static void setDefaultDatabasePath(const std::string& path);
    static std::string defaultDatabasePath();

    // Explicitly close the database connection
    void close();

    // Save result API
    TransactionSaveResult getLastSaveResult() const;
    std::string getLastError() const;

private:
    std::string databasePath;
    sqlite3* database = nullptr;
    bool initialized = false;
    mutable bool runtimeReadFailure_ = false;
    TransactionSaveResult lastSaveResult = TransactionSaveResult::DATABASE_ERROR; // Default to DATABASE_ERROR

    mutable std::mutex mutex_; // Mutex for protecting repository operations
    mutable std::mutex dbMutex_; // Mutex for database connection
    mutable std::string lastError_;

    bool configureConnection();
    bool executeSchema();
    bool executeSql(const char* sql);
    bool pragmaForeignKeys(bool enable);

    // Integrity and transaction helpers
    bool beginTransaction();
    bool commitTransaction();
    bool rollbackTransaction();
    bool updateTransactionStatusImpl(
        const Transaction& transaction,
        const TransactionStatus* expectedStatus,
        const std::string* replayRequestId = nullptr,
        ReplayState replayState = ReplayState::PROCESSING,
        const std::string& responseStatus = {},
        const std::string& adapterResultCode = {},
        const std::string& adapterMessage = {},
        const std::string& unknownReason = {});
    bool loadTransactionByIdUnlocked(
        const std::string& transactionId,
        Transaction& transaction) const;
    bool findTransactionByExternalReferenceUnlocked(
        const std::string& externalReference,
        Transaction& transaction) const;
};

