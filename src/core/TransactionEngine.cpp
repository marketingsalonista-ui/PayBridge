#include "TransactionEngine.h"
#include "TransactionLogger.h"
#include "TransactionRepository.h"

#include "../money/AmountParser.h"

#include <iomanip>
#include <sstream>
#include <filesystem>
#include <fstream>
#include <array>
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <objbase.h>
#include <utility>

namespace
{
std::string hashHex(const std::string& value)
{
    std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
    SHA256(reinterpret_cast<const unsigned char*>(value.data()), value.size(), digest.data());
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (unsigned char byte : digest)
        output << std::setw(2) << static_cast<unsigned int>(byte);
    return output.str();
}

std::string fileHash(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open())
        return {};
    EVP_MD_CTX* context = EVP_MD_CTX_new();
    if (!context)
        return {};
    bool success = EVP_DigestInit_ex(context, EVP_sha256(), nullptr) == 1;
    std::array<char, 8192> buffer{};
    while (success && file.good())
    {
        file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = file.gcount();
        if (count > 0)
            success = EVP_DigestUpdate(context, buffer.data(), static_cast<std::size_t>(count)) == 1;
    }
    if (file.bad())
        success = false;

    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digestLength = 0;
    success = success && EVP_DigestFinal_ex(context, digest.data(), &digestLength) == 1;
    EVP_MD_CTX_free(context);
    if (!success)
        return {};

    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (unsigned int index = 0; index < digestLength; ++index)
        output << std::setw(2) << static_cast<unsigned int>(digest[index]);
    return output.str();
}
}

TransactionEngine::TransactionEngine()
    : TransactionEngine(TransactionRepository::defaultDatabasePath(),
                        TransactionLogger::filePath())
{
}

TransactionEngine::TransactionEngine(
    std::string databasePath,
    std::string legacyCsvPath)
    : repository(std::make_unique<TransactionRepository>(databasePath)),
      legacyCsvPath(std::move(legacyCsvPath))
{
}

TransactionEngine::~TransactionEngine() = default;

bool TransactionEngine::initialize()
{
    std::lock_guard<std::mutex> lock(engineMutex);
    if (ready)
    {
        if (repository->isReadyForReads())
            return true;
        persistenceErrorMessage = "SQLite transaction data is no longer readable";
        return false;
    }

    persistenceErrorMessage.clear();
    if (!repository->initialize())
    {
        persistenceErrorMessage = repository->getLastError();
        if (persistenceErrorMessage.empty())
            persistenceErrorMessage = "SQLite initialization failed";
        return false;
    }

    if (!legacyCsvPath.empty())
    {
        const auto csv = TransactionLogger::readTransactionsFromFile(legacyCsvPath);
        if (csv.sourceExists)
        {
            std::string parsedSource = csv.rawHeader;
            for (const auto& row : csv.records)
                parsedSource += row.rawRecord;
            const std::string sourceHash = hashHex(parsedSource);
            const std::string diskHash = fileHash(legacyCsvPath);
            if (diskHash.empty() || sourceHash != diskHash)
            {
                persistenceErrorMessage = "Legacy CSV changed or could not be hashed while being read";
                return false;
            }

            std::vector<LegacyCsvRecord> records;
            records.reserve(csv.records.size() + (csv.headerValid ? 0 : 1));
            if (!csv.headerValid)
            {
                LegacyCsvRecord invalidHeader;
                invalidHeader.rowNumber = 1;
                invalidHeader.rawRecord = csv.diagnostic.empty()
                    ? "<invalid CSV header>" : csv.diagnostic;
                invalidHeader.rawRecordHash = hashHex(invalidHeader.rawRecord);
                invalidHeader.diagnostic = csv.diagnostic.empty()
                    ? "CSV header is missing or malformed" : csv.diagnostic;
                records.push_back(std::move(invalidHeader));
            }

            for (const auto& csvRecord : csv.records)
            {
                LegacyCsvRecord record;
                record.rowNumber = csvRecord.rowNumber;
                record.rawRecord = csvRecord.rawRecord;
                record.rawRecordHash = hashHex(csvRecord.rawRecord);
                const bool legacySevenColumnRow = csvRecord.columnCount == 7;
                record.valid = csvRecord.valid;
                record.auditOnly = csvRecord.auditOnly;
                record.diagnostic = csvRecord.diagnostic;
                record.transaction = csvRecord.transaction;

                if (record.valid)
                {
                    Transaction& transaction = record.transaction;
                    transaction.money.units = transaction.amountMinor;
                    if (transaction.asset.isFiat())
                    {
                        const auto fiat = assetRegistry.findFiat(transaction.asset.code);
                        if (!fiat)
                        {
                            record.valid = false;
                            record.diagnostic = "Legacy fiat currency is not registered: " + transaction.asset.code;
                        }
                        else
                        {
                            if (!legacySevenColumnRow &&
                                transaction.money.precision != fiat->decimalPrecision)
                            {
                                record.valid = false;
                                record.diagnostic = "CSV fiat precision conflicts with the registered currency";
                            }
                            transaction.asset = *fiat;
                            transaction.currency = fiat->code;
                            transaction.money.precision = fiat->decimalPrecision;
                        }
                    }
                    else
                    {
                        const auto configured = assetRegistry.resolveDigitalAsset(
                            transaction.asset.code,
                            transaction.asset.network,
                            transaction.asset.contractAddress);
                        if (!configured || configured->decimalPrecision != transaction.money.precision)
                        {
                            record.valid = false;
                            record.diagnostic = "Legacy digital asset is not configured or has a precision conflict";
                        }
                        else
                        {
                            transaction.asset = *configured;
                        }
                    }
                }
                records.push_back(std::move(record));
            }

            std::string sourceName;
            try
            {
                sourceName = std::filesystem::absolute(legacyCsvPath).lexically_normal().string();
            }
            catch (...)
            {
                sourceName = legacyCsvPath;
            }

            const LegacyMigrationResult migration =
                repository->importLegacyCsv(sourceName, sourceHash, records);
            if (!migration.success)
            {
                std::ostringstream message;
                message << "Legacy CSV migration did not complete";
                for (const auto& diagnostic : migration.diagnostics)
                    message << "\n - " << diagnostic;
                persistenceErrorMessage = message.str();
                return false;
            }
        }
        else if (!csv.diagnostic.empty())
        {
            persistenceErrorMessage = "Could not read legacy CSV: " + csv.diagnostic;
            return false;
        }
    }

    if (!repository->recoverExpiredProcessingRequests())
    {
        persistenceErrorMessage = repository->getLastError();
        if (persistenceErrorMessage.empty())
            persistenceErrorMessage = "Could not recover expired in-flight payment requests";
        return false;
    }

    if (!restoreTransactions())
        return false;
    ready = true;
    return true;
}

bool TransactionEngine::isReady() const noexcept
{
    std::lock_guard<std::mutex> lock(engineMutex);
    return ready && repository && repository->isReadyForReads();
}

std::string TransactionEngine::persistenceError() const
{
    std::lock_guard<std::mutex> lock(engineMutex);
    return persistenceErrorMessage;
}

TransactionPage TransactionEngine::listTransactions(
    std::size_t pageSize,
    std::optional<TransactionStatus> statusFilter,
    std::optional<TransactionCursor> cursor) const
{
    if (!isReady()) return {};
    return repository->listTransactions(pageSize, statusFilter, std::move(cursor));
}

std::optional<TransactionReadRecord> TransactionEngine::findTransactionById(
    const std::string& transactionId) const
{
    if (!isReady()) return std::nullopt;
    return repository->findTransactionById(transactionId);
}

paybridge::assets::AssetRegistry& TransactionEngine::assets()
{
    return assetRegistry;
}

bool TransactionEngine::restoreTransactions()
{
    const auto savedTransactions = repository->loadTransactions();
    const std::string loadError = repository->getLastError();
    if (!loadError.empty())
    {
        persistenceErrorMessage = loadError;
        return false;
    }

    for (Transaction transaction : savedTransactions)
    {
        if (!transaction.asset.isComplete() ||
            !assetRegistry.contains(transaction.asset))
        {
            persistenceErrorMessage = "SQLite contains a transaction whose asset is not configured: " +
                transaction.externalReference;
            return false;
        }
        transaction.money.units = transaction.amountMinor;
        transactionsByReference[transaction.externalReference] = transaction;
    }
    return true;
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
    if (!initialize())
        return Transaction{.externalReference = externalReference,
                           .amountMinor = amountMinor,
                           .currency = currency,
                           .status = TransactionStatus::FAILED};

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

ReplayAwareTransactionResult TransactionEngine::createTransaction(
    const paybridge::money::Money& money,
    const paybridge::assets::Asset& asset,
    const std::string& externalReference,
    const RequestReplayData& replayData)
{
    ReplayAwareTransactionResult result;
    if (!initialize()) { persistenceErrorMessage = repository->getLastError(); return result; }
    std::lock_guard<std::mutex> lock(engineMutex);
    if (!money.isPositive() || externalReference.empty() || !asset.isComplete() ||
        !assetRegistry.contains(asset) || money.precision != asset.decimalPrecision)
    { result.code = ReplayRegistrationCode::DATABASE_ERROR; return result; }
    Transaction candidate;
    candidate.externalReference=externalReference; candidate.amountMinor=money.units; candidate.money=money; candidate.asset=asset; candidate.currency=asset.isFiat()?asset.code:std::string{}; candidate.status=TransactionStatus::CREATED;
    GUID guid{}; if (CoCreateGuid(&guid) != S_OK) { persistenceErrorMessage="Transaction identifier generation failed"; return result; }
    std::ostringstream id; id << "PB-" << std::uppercase << std::hex << std::setfill('0') << std::setw(8) << guid.Data1 << std::setw(4) << guid.Data2 << std::setw(4) << guid.Data3;
    for (int i=0;i<8;++i) id << std::setw(2) << static_cast<unsigned int>(guid.Data4[i]);
    candidate.transactionId=id.str();
    result=repository->registerPaymentRequest(candidate,replayData);
    if (result.code==ReplayRegistrationCode::DATABASE_ERROR) persistenceErrorMessage=repository->getLastError();
    if (result.code==ReplayRegistrationCode::NEW_REQUEST || result.code==ReplayRegistrationCode::EXISTING_EXTERNAL_REFERENCE || result.code==ReplayRegistrationCode::IDENTICAL_RETRY)
        transactionsByReference[result.transaction.externalReference]=result.transaction;
    return result;
}

bool TransactionEngine::bindSimulatedTerminal(const std::string& requestId,
    const std::string& terminalId, const std::string& contractVersion)
{
    if (!initialize() || requestId.empty() || terminalId.empty() || contractVersion.empty())
        return false;
    return repository->bindSimulatedTerminal(requestId, terminalId, contractVersion);
}

Transaction TransactionEngine::createTransaction(
    const paybridge::money::Money& money,
    const paybridge::assets::Asset& asset,
    const std::string& externalReference)
{
    if (!initialize())
    {
        Transaction failed;
        failed.amountMinor = money.units;
        failed.currency = asset.isFiat() ? asset.code : std::string{};
        failed.externalReference = externalReference;
        failed.asset = asset;
        failed.money = money;
        failed.status = TransactionStatus::FAILED;
        return failed;
    }

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
        Transaction canonical;
        if (!repository->findByExternalReference(externalReference, canonical))
        {
            transaction.status = TransactionStatus::FAILED;
            persistenceErrorMessage = repository->getLastError();
            return transaction;
        }
        existing->second = canonical;
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

    const TransactionSaveResult saveResult = repository->saveTransaction(transaction);
    if (saveResult == TransactionSaveResult::EXISTING_IDENTICAL)
    {
        Transaction canonical;
        if (repository->findByExternalReference(externalReference, canonical) &&
            canonical.money.units == money.units && canonical.asset == asset)
        {
            transactionsByReference[externalReference] = canonical;
            return canonical;
        }
        transaction.status = TransactionStatus::FAILED;
        persistenceErrorMessage = "SQLite reported an identical reference but the canonical row could not be loaded";
        return transaction;
    }
    if (saveResult != TransactionSaveResult::NEW_TRANSACTION)
    {
        transaction.status = saveResult == TransactionSaveResult::CONFLICT
            ? TransactionStatus::INVALID : TransactionStatus::FAILED;
        persistenceErrorMessage = saveResult == TransactionSaveResult::CONFLICT
            ? "External reference already belongs to a different payment"
            : repository->getLastError();
        return transaction;
    }

    transactionsByReference.emplace(externalReference, transaction);
    return transaction;
}

bool TransactionEngine::updateStatus(
    Transaction& transaction,
    TransactionStatus newStatus)
{
    if (!initialize())
        return false;
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
    if (!repository->updateTransactionStatus(
            updatedTransaction, storedTransaction.status))
    {
        persistenceErrorMessage = repository->getLastError();
        Transaction canonical;
        if (repository->findByExternalReference(
                storedTransaction.externalReference, canonical) &&
            canonical.transactionId == storedTransaction.transactionId)
        {
            existing->second = canonical;
        }
        return false;
    }

    existing->second = updatedTransaction;

    // Synchronize the caller's transaction object.
    transaction = updatedTransaction;

    return true;
}

bool TransactionEngine::updateStatusForRequest(Transaction& transaction, TransactionStatus newStatus, const std::string& requestId, ReplayState replayState, const std::string& responseStatus, const std::string& adapterResultCode, const std::string& adapterMessage, const std::string& unknownReason)
{
    if (!initialize()) return false;
    std::lock_guard<std::mutex> lock(engineMutex);
    auto it=transactionsByReference.find(transaction.externalReference);
    if(it==transactionsByReference.end() || it->second.transactionId!=transaction.transactionId || it->second.status!=transaction.status || !isValidTransition(it->second.status,newStatus)) return false;
    Transaction updated=it->second; updated.status=newStatus;
    if(!transaction.adapterId.empty()) updated.adapterId=transaction.adapterId;
    if(!repository->updateTransactionStatusForRequest(updated,it->second.status,requestId,replayState,responseStatus,adapterResultCode,adapterMessage,unknownReason)) { persistenceErrorMessage=repository->getLastError(); return false; }
    it->second=updated; transaction=updated; return true;
}

bool TransactionEngine::isValidTransition(
    TransactionStatus currentStatus,
    TransactionStatus newStatus)
{
    switch (currentStatus)
    {
        case TransactionStatus::CREATED:
            return newStatus == TransactionStatus::SENT ||
                   newStatus == TransactionStatus::FAILED ||
                   newStatus == TransactionStatus::UNKNOWN;

        case TransactionStatus::SENT:
            return newStatus == TransactionStatus::RECEIVED ||
                   newStatus == TransactionStatus::FAILED ||
                   newStatus == TransactionStatus::UNKNOWN;

        case TransactionStatus::RECEIVED:
            return newStatus == TransactionStatus::APPROVED ||
                   newStatus == TransactionStatus::DECLINED ||
                   newStatus == TransactionStatus::FAILED ||
                   newStatus == TransactionStatus::UNKNOWN;

        case TransactionStatus::UNKNOWN:
            return newStatus == TransactionStatus::RECEIVED ||
                   newStatus == TransactionStatus::APPROVED ||
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
        case TransactionStatus::UNKNOWN: return "UNKNOWN";
        case TransactionStatus::INVALID: return "INVALID";
        default: return "UNKNOWN";
    }
}
