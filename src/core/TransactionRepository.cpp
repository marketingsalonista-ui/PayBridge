#include "TransactionRepository.h"

#include <filesystem>
#include <mutex>
#include <sstream>
#include <chrono>
#include <cstdlib>
#include <array>
#include <algorithm>
#include <unordered_map>
#include <openssl/sha.h>
#include <iomanip>

namespace
{
constexpr const char* SCHEMA_VERSION = "2";
constexpr const char* PREVIOUS_SCHEMA_VERSION = "1";
constexpr std::size_t MAX_TRANSACTION_PAGE_SIZE = 100;
constexpr std::size_t MAX_CURSOR_TRANSACTION_ID_LENGTH = 256;
std::mutex databasePathMutex;
std::mutex initializationMutex;
std::string defaultDatabasePathValue = "data/paybridge.db";

int bindText(sqlite3_stmt* statement, int index, const std::string& value)
{
    return sqlite3_bind_text(statement, index, value.c_str(),
                             static_cast<int>(value.size()), SQLITE_TRANSIENT);
}

std::string sqliteText(sqlite3_stmt* statement, int column)
{
    const unsigned char* text = sqlite3_column_text(statement, column);
    return text ? reinterpret_cast<const char*>(text) : std::string{};
}

std::int64_t unixNow()
{
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string digestHex(const std::string& value)
{
    std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
    SHA256(reinterpret_cast<const unsigned char*>(value.data()), value.size(), digest.data());
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (unsigned char byte : digest)
        output << std::setw(2) << static_cast<unsigned int>(byte);
    return output.str();
}

bool samePayment(const Transaction& left, const Transaction& right)
{
    return left.externalReference == right.externalReference &&
           left.money.units == right.money.units &&
           left.money.precision == right.money.precision &&
           left.asset == right.asset &&
           left.currency == right.currency;
}

bool statusCanAdvanceTo(TransactionStatus source, TransactionStatus target)
{
    if (source == target)
        return true;
    const std::array<TransactionStatus, 8> states = {
        TransactionStatus::CREATED, TransactionStatus::SENT,
        TransactionStatus::RECEIVED, TransactionStatus::APPROVED,
        TransactionStatus::DECLINED, TransactionStatus::FAILED,
        TransactionStatus::UNKNOWN, TransactionStatus::INVALID};
    std::vector<TransactionStatus> pending{source};
    std::vector<TransactionStatus> visited{source};
    while (!pending.empty())
    {
        const TransactionStatus current = pending.back();
        pending.pop_back();
        for (const TransactionStatus next : states)
        {
            if (!TransactionEngine::isValidTransition(current, next))
                continue;
            if (next == target)
                return true;
            if (std::find(visited.begin(), visited.end(), next) == visited.end())
            {
                visited.push_back(next);
                pending.push_back(next);
            }
        }
    }
    return false;
}

bool statusTextCanAdvanceTo(TransactionStatus source, const std::string& target)
{
    const std::array<TransactionStatus, 8> states = {
        TransactionStatus::CREATED, TransactionStatus::SENT,
        TransactionStatus::RECEIVED, TransactionStatus::APPROVED,
        TransactionStatus::DECLINED, TransactionStatus::FAILED,
        TransactionStatus::UNKNOWN, TransactionStatus::INVALID};
    for (const TransactionStatus candidate : states)
    {
        if (TransactionEngine::statusToString(candidate) == target &&
            statusCanAdvanceTo(source, candidate))
            return true;
    }
    return false;
}

const char* statusDatabaseText(TransactionStatus status)
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
    case TransactionStatus::UNKNOWN: return "UNKNOWN";
    }
    return nullptr;
}

bool readTransactionRecord(sqlite3_stmt* statement, TransactionReadRecord& record)
{
    record.transactionId = sqliteText(statement, 0);
    record.money.units = sqlite3_column_int64(statement, 1);
    record.money.precision = static_cast<std::uint8_t>(sqlite3_column_int(statement, 2));
    const std::string currency = sqliteText(statement, 3);
    const std::string assetType = sqliteText(statement, 4);
    record.asset.code = sqliteText(statement, 5);
    record.asset.decimalPrecision = record.money.precision;
    if (assetType == "FIAT")
    {
        record.asset.type = paybridge::assets::AssetType::FIAT;
        record.asset.code = currency.empty() ? record.asset.code : currency;
    }
    else if (assetType == "DIGITAL_ASSET")
    {
        record.asset.type = paybridge::assets::AssetType::DIGITAL_ASSET;
        record.asset.network = sqliteText(statement, 6);
        record.asset.contractAddress = sqliteText(statement, 7);
    }
    else return false;

    const std::string state = sqliteText(statement, 8);
    if (state == "CREATED") record.status = TransactionStatus::CREATED;
    else if (state == "SENT") record.status = TransactionStatus::SENT;
    else if (state == "RECEIVED") record.status = TransactionStatus::RECEIVED;
    else if (state == "APPROVED") record.status = TransactionStatus::APPROVED;
    else if (state == "DECLINED") record.status = TransactionStatus::DECLINED;
    else if (state == "FAILED") record.status = TransactionStatus::FAILED;
    else if (state == "UNKNOWN") record.status = TransactionStatus::UNKNOWN;
    else if (state == "INVALID") record.status = TransactionStatus::INVALID;
    else return false;
    record.createdAtUnix = sqlite3_column_int64(statement, 9);
    record.updatedAtUnix = sqlite3_column_int64(statement, 10);
    record.terminalId = sqliteText(statement, 11);
    return true;
}
}

TransactionRepository::TransactionRepository(const std::string& databasePath)
    : databasePath(databasePath)
{
}

TransactionRepository::~TransactionRepository()
{
    close();
}

void TransactionRepository::setDefaultDatabasePath(const std::string& path)
{
    std::lock_guard<std::mutex> lock(databasePathMutex);
    defaultDatabasePathValue = path;
}

std::string TransactionRepository::defaultDatabasePath()
{
    std::lock_guard<std::mutex> lock(databasePathMutex);
    return defaultDatabasePathValue;
}

bool TransactionRepository::isInitialized() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    return initialized && database;
}

bool TransactionRepository::isReadyForReads() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized || !database || runtimeReadFailure_)
        return false;

    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database,
            "SELECT transaction_id FROM transactions LIMIT 1", -1,
            &statement, nullptr) != SQLITE_OK)
    {
        lastError_ = sqlite3_errmsg(database);
        runtimeReadFailure_ = true;
        return false;
    }

    const int result = sqlite3_step(statement);
    sqlite3_finalize(statement);
    if (result != SQLITE_ROW && result != SQLITE_DONE)
    {
        lastError_ = sqlite3_errmsg(database);
        runtimeReadFailure_ = true;
        return false;
    }
    return true;
}

bool TransactionRepository::executeSql(const char* sql)
{
    char* error = nullptr;
    const int result = sqlite3_exec(database, sql, nullptr, nullptr, &error);

    if (result != SQLITE_OK)
    {
        lastError_ = sqlite3_errmsg(database);
        if (error)
            sqlite3_free(error);
        return false;
    }

    return true;
}

bool TransactionRepository::executeSchema()
{
    const char* schema = R"SQL(
CREATE TABLE IF NOT EXISTS transactions (
    transaction_id TEXT PRIMARY KEY NOT NULL CHECK (length(transaction_id) > 0),
    external_reference TEXT NOT NULL UNIQUE CHECK (length(external_reference) > 0),
    amount_units INTEGER NOT NULL CHECK (amount_units > 0),
    money_precision INTEGER NOT NULL CHECK (money_precision >= 0 AND money_precision <= 18),
    currency TEXT,
    asset_type TEXT NOT NULL CHECK (asset_type IN ('FIAT', 'DIGITAL_ASSET')),
    asset_code TEXT NOT NULL CHECK (length(asset_code) > 0),
    asset_network TEXT,
    contract_address TEXT,
    lifecycle_state TEXT NOT NULL CHECK (lifecycle_state IN ('CREATED','SENT','RECEIVED','APPROVED','DECLINED','FAILED','INVALID','UNKNOWN')),
    legacy_imported INTEGER NOT NULL DEFAULT 0 CHECK (legacy_imported IN (0,1)),
    created_at_unix INTEGER NOT NULL CHECK (created_at_unix >= 0),
    updated_at_unix INTEGER NOT NULL CHECK (updated_at_unix >= created_at_unix),
    UNIQUE (transaction_id, external_reference)
);

CREATE TABLE IF NOT EXISTS replay_records (
    request_id TEXT PRIMARY KEY NOT NULL,
    transaction_id TEXT NOT NULL,
    external_reference TEXT NOT NULL,
    canonical_request_hash TEXT NOT NULL,
    payment_fingerprint TEXT NOT NULL,
    request_timestamp_unix INTEGER NOT NULL CHECK (request_timestamp_unix >= 0),
    request_expiry_unix INTEGER NOT NULL CHECK (request_expiry_unix >= 0 AND request_expiry_unix > request_timestamp_unix),
    replay_state TEXT NOT NULL CHECK (replay_state IN ('RESERVED','PROCESSING','COMPLETED','FAILED','UNKNOWN')),
    processing_started_at_unix INTEGER,
    terminal_at_unix INTEGER,
    response_status TEXT,
    adapter_result_code TEXT,
    adapter_message TEXT,
    unknown_reason TEXT,
    terminal_id TEXT,
    simulation_contract TEXT,
    created_at_unix INTEGER NOT NULL CHECK (created_at_unix >= 0),
    updated_at_unix INTEGER NOT NULL CHECK (updated_at_unix >= created_at_unix),
    FOREIGN KEY (transaction_id, external_reference) REFERENCES transactions(transaction_id, external_reference) ON UPDATE RESTRICT ON DELETE RESTRICT
);

CREATE TABLE IF NOT EXISTS transaction_lifecycle_events (
    event_id INTEGER PRIMARY KEY AUTOINCREMENT,
    transaction_id TEXT NOT NULL,
    replay_request_id TEXT,
    from_status TEXT CHECK (from_status IS NULL OR from_status IN ('CREATED','SENT','RECEIVED','APPROVED','DECLINED','FAILED','INVALID','UNKNOWN')),
    to_status TEXT NOT NULL CHECK (to_status IN ('CREATED','SENT','RECEIVED','APPROVED','DECLINED','FAILED','INVALID','UNKNOWN')),
    event_type TEXT NOT NULL CHECK (event_type IN ('RESERVED','LIFECYCLE','ADAPTER_ACCEPTED','ADAPTER_REJECTED','UNKNOWN','LEGACY_IMPORT')),
    event_time_unix INTEGER NOT NULL CHECK (event_time_unix >= 0),
    reason TEXT,
    FOREIGN KEY (transaction_id) REFERENCES transactions(transaction_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    FOREIGN KEY (replay_request_id) REFERENCES replay_records(request_id) ON UPDATE RESTRICT ON DELETE RESTRICT
);

CREATE TABLE IF NOT EXISTS migration_legacy_rows (
    source_name TEXT NOT NULL,
    source_row_number INTEGER NOT NULL CHECK (source_row_number >= 1),
    transaction_id TEXT,
    external_reference TEXT,
    amount_units INTEGER,
    currency TEXT,
    money_precision INTEGER,
    asset_type TEXT,
    asset_code TEXT,
    asset_network TEXT,
    contract_address TEXT,
    lifecycle_state TEXT,
    raw_record TEXT NOT NULL,
    raw_record_hash TEXT NOT NULL,
    PRIMARY KEY (source_name, source_row_number)
);

CREATE TABLE IF NOT EXISTS migration_batches (
    batch_id TEXT PRIMARY KEY NOT NULL,
    source_name TEXT NOT NULL,
    source_version TEXT NOT NULL,
    source_row_count INTEGER NOT NULL CHECK (source_row_count >= 0),
    imported_row_count INTEGER NOT NULL CHECK (imported_row_count >= 0),
    reconciled_row_count INTEGER NOT NULL CHECK (reconciled_row_count >= 0),
    conflict_count INTEGER NOT NULL CHECK (conflict_count >= 0),
    status TEXT NOT NULL CHECK (status IN ('RUNNING','VALIDATED','COMPLETE','FAILED')),
    source_hash TEXT,
    started_at_unix INTEGER NOT NULL CHECK (started_at_unix >= 0),
    completed_at_unix INTEGER CHECK (completed_at_unix IS NULL OR completed_at_unix >= started_at_unix)
);

CREATE TABLE IF NOT EXISTS schema_metadata (
    metadata_key TEXT PRIMARY KEY NOT NULL,
    metadata_value TEXT NOT NULL
);

CREATE INDEX IF NOT EXISTS ix_replay_transaction ON replay_records(transaction_id);
CREATE INDEX IF NOT EXISTS ix_replay_external_reference ON replay_records(external_reference);
CREATE INDEX IF NOT EXISTS ix_replay_state ON replay_records(replay_state);
CREATE INDEX IF NOT EXISTS ix_replay_expiry ON replay_records(request_expiry_unix);
CREATE INDEX IF NOT EXISTS ix_lifecycle_transaction ON transaction_lifecycle_events(transaction_id, event_id);
CREATE INDEX IF NOT EXISTS ix_lifecycle_request ON transaction_lifecycle_events(replay_request_id);
CREATE INDEX IF NOT EXISTS ix_migration_external_reference ON migration_legacy_rows(external_reference);

)SQL";

    return executeSql(schema);
}

bool TransactionRepository::initialize()
{
    std::lock_guard<std::mutex> initializeLock(initializationMutex);
    std::lock_guard<std::mutex> lock(mutex_);
    if (database)
        return initialized;
    initialized = false;
    runtimeReadFailure_ = false;
    lastError_.clear();

    auto fail = [this]() {
        if (database && sqlite3_get_autocommit(database) == 0)
            sqlite3_exec(database, "ROLLBACK", nullptr, nullptr, nullptr);
        if (database)
        {
            sqlite3_close_v2(database);
            database = nullptr;
        }
        initialized = false;
        return false;
    };

    std::filesystem::path path(databasePath);
    std::error_code directoryError;

    if (path.has_parent_path())
        std::filesystem::create_directories(path.parent_path(), directoryError);

    if (directoryError) { lastError_ = directoryError.message(); return false; }

    if (sqlite3_open_v2(databasePath.c_str(), &database,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK)
    {
        lastError_ = database ? sqlite3_errmsg(database) : "SQLite open failed";
        return fail();
    }
    sqlite3_busy_timeout(database, 5000);

    // Inspect existing metadata before any DDL, so a future database is never altered.
    bool hasMetadata = false;
    sqlite3_stmt* check = nullptr;
    if (sqlite3_prepare_v2(database,
            "SELECT 1 FROM sqlite_master WHERE type='table' AND name='schema_metadata'", -1, &check, nullptr) != SQLITE_OK)
    { lastError_ = sqlite3_errmsg(database); return fail(); }
    hasMetadata = sqlite3_step(check) == SQLITE_ROW;
    sqlite3_finalize(check);
    if (hasMetadata)
    {
        sqlite3_stmt* versionStmt = nullptr;
        if (sqlite3_prepare_v2(database, "SELECT metadata_value FROM schema_metadata WHERE metadata_key='schema_version'", -1, &versionStmt, nullptr) != SQLITE_OK)
        { lastError_ = sqlite3_errmsg(database); return fail(); }
        const int rc = sqlite3_step(versionStmt);
        const std::string version = rc == SQLITE_ROW ? sqliteText(versionStmt, 0) : std::string{};
        sqlite3_finalize(versionStmt);
        if (rc != SQLITE_ROW ||
            (version != SCHEMA_VERSION && version != PREVIOUS_SCHEMA_VERSION))
        { lastError_ = "Unsupported or missing SQLite schema version"; return fail(); }
    }

    if (!executeSql("PRAGMA journal_mode=WAL;") || !executeSql("PRAGMA synchronous=FULL;") ||
        !executeSql("PRAGMA foreign_keys=ON;"))
    { lastError_ = sqlite3_errmsg(database); return fail(); }
    auto pragmaText = [this](const char* sql, const char* expected) {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(database, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
        const bool ok = sqlite3_step(stmt) == SQLITE_ROW && sqliteText(stmt, 0) == expected;
        sqlite3_finalize(stmt);
        return ok;
    };
    auto pragmaInt = [this](const char* sql, int expected) {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(database, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
        const bool ok = sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) == expected;
        sqlite3_finalize(stmt);
        return ok;
    };
    if (!pragmaText("PRAGMA journal_mode", "wal") || !pragmaInt("PRAGMA synchronous", 2) ||
        !pragmaInt("PRAGMA foreign_keys", 1))
    { lastError_ = "Required SQLite connection settings could not be verified"; return fail(); }

    if (!executeSql("BEGIN IMMEDIATE;"))
    { lastError_ = sqlite3_errmsg(database); return fail(); }

    if (!executeSchema())
    { lastError_ = sqlite3_errmsg(database); return fail(); }

    auto hasColumn = [this](const char* table, const char* column)
    {
        const std::string sql = std::string("PRAGMA table_info(") + table + ");";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(database, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
            return false;
        bool found = false;
        while (sqlite3_step(stmt) == SQLITE_ROW)
            if (sqliteText(stmt, 1) == column) found = true;
        sqlite3_finalize(stmt);
        return found;
    };
    if (!hasColumn("replay_records", "terminal_id") &&
        !executeSql("ALTER TABLE replay_records ADD COLUMN terminal_id TEXT;"))
    { lastError_ = sqlite3_errmsg(database); return fail(); }
    if (!hasColumn("replay_records", "simulation_contract") &&
        !executeSql("ALTER TABLE replay_records ADD COLUMN simulation_contract TEXT;"))
    { lastError_ = sqlite3_errmsg(database); return fail(); }

    if (!hasMetadata && !executeSql("INSERT INTO schema_metadata(metadata_key, metadata_value) VALUES ('schema_version','1');"))
    { lastError_ = sqlite3_errmsg(database); return fail(); }
    if (!executeSql("UPDATE schema_metadata SET metadata_value='2' WHERE metadata_key='schema_version';"))
    { lastError_ = sqlite3_errmsg(database); return fail(); }

    if (!executeSql("COMMIT;"))
    { lastError_ = sqlite3_errmsg(database); return fail(); }

    sqlite3_stmt* integrity = nullptr;
    if (sqlite3_prepare_v2(database, "PRAGMA quick_check", -1, &integrity, nullptr) != SQLITE_OK)
    { lastError_ = sqlite3_errmsg(database); return fail(); }
    bool integrityOk = true;
    int integrityRc = SQLITE_OK;
    while ((integrityRc = sqlite3_step(integrity)) == SQLITE_ROW)
        if (sqliteText(integrity, 0) != "ok") integrityOk = false;
    sqlite3_finalize(integrity);
    if (!integrityOk || integrityRc != SQLITE_DONE)
    { lastError_ = "SQLite quick_check failed"; return fail(); }

    initialized = true;
    return true;
}

TransactionSaveResult TransactionRepository::saveTransaction(const Transaction& transaction)
{
    std::lock_guard<std::mutex> lock(mutex_);
    lastError_.clear();
    lastSaveResult = TransactionSaveResult::DATABASE_ERROR;
    if (!initialized || !database || transaction.transactionId.empty() ||
        transaction.externalReference.empty() || transaction.money.units <= 0)
    {
        return TransactionSaveResult::DATABASE_ERROR;
    }

    if (!executeSql("BEGIN IMMEDIATE;")) return lastSaveResult;
    auto rollback = [this]() {
        if (database && lastError_.empty())
            lastError_ = sqlite3_errmsg(database);
        rollbackTransaction();
        return lastSaveResult;
    };

    sqlite3_stmt* existing = nullptr;
    const char* compareSql = R"SQL(
SELECT amount_units, money_precision, COALESCE(currency,''), asset_type,
       asset_code, COALESCE(asset_network,''), COALESCE(contract_address,'')
FROM transactions WHERE external_reference=?;
)SQL";
    if (sqlite3_prepare_v2(database, compareSql, -1, &existing, nullptr) != SQLITE_OK) return rollback();
    bindText(existing, 1, transaction.externalReference);
    const int existingRc = sqlite3_step(existing);
    if (existingRc == SQLITE_ROW)
    {
        const bool identical =
            sqlite3_column_int64(existing, 0) == transaction.money.units &&
            sqlite3_column_int(existing, 1) == transaction.money.precision &&
            sqliteText(existing, 2) == (transaction.asset.isFiat() ? transaction.currency : "") &&
            sqliteText(existing, 3) == (transaction.asset.isFiat() ? "FIAT" : "DIGITAL_ASSET") &&
            sqliteText(existing, 4) == transaction.asset.code &&
            sqliteText(existing, 5) == (transaction.asset.isFiat() ? "" : transaction.asset.network) &&
            sqliteText(existing, 6) == (transaction.asset.isFiat() ? "" : transaction.asset.contractAddress);
        sqlite3_finalize(existing);
        if (!rollbackTransaction()) return TransactionSaveResult::DATABASE_ERROR;
        lastSaveResult = identical ? TransactionSaveResult::EXISTING_IDENTICAL : TransactionSaveResult::CONFLICT;
        return lastSaveResult;
    }
    sqlite3_finalize(existing);
    if (existingRc != SQLITE_DONE) return rollback();

    sqlite3_stmt* statement = nullptr;
    const char* sql = R"SQL(
INSERT INTO transactions (
    transaction_id, external_reference, amount_units, money_precision,
    currency, asset_type, asset_code, asset_network, contract_address,
    lifecycle_state, legacy_imported, created_at_unix, updated_at_unix
) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 0, ?, ?)
)SQL";

    if (sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) != SQLITE_OK)
        return rollback();

    const std::int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    bindText(statement, 1, transaction.transactionId);
    bindText(statement, 2, transaction.externalReference);
    sqlite3_bind_int64(statement, 3, transaction.money.units);
    sqlite3_bind_int(statement, 4, transaction.money.precision);

    if (transaction.asset.isFiat())
    {
        sqlite3_bind_text(statement, 5, transaction.currency.c_str(), -1, SQLITE_TRANSIENT);
        bindText(statement, 6, "FIAT");
        bindText(statement, 7, transaction.asset.code);
        sqlite3_bind_null(statement, 8);
        sqlite3_bind_null(statement, 9);
    }
    else
    {
        sqlite3_bind_null(statement, 5);
        bindText(statement, 6, "DIGITAL_ASSET");
        bindText(statement, 7, transaction.asset.code);
        bindText(statement, 8, transaction.asset.network);
        bindText(statement, 9, transaction.asset.contractAddress);
    }

    bindText(statement, 10, TransactionEngine::statusToString(transaction.status));
    sqlite3_bind_int64(statement, 11, now);
    sqlite3_bind_int64(statement, 12, now);

    const bool success = sqlite3_step(statement) == SQLITE_DONE;
    sqlite3_finalize(statement);

    if (!success) return rollback();

    statement = nullptr;
    const char* eventSql = R"SQL(
INSERT INTO transaction_lifecycle_events (
    transaction_id, replay_request_id, from_status, to_status,
    event_type, event_time_unix, reason
) VALUES (?, NULL, NULL, ?, 'LIFECYCLE', ?, NULL);
)SQL";

    if (sqlite3_prepare_v2(database, eventSql, -1, &statement, nullptr) != SQLITE_OK)
        return rollback();

    bindText(statement, 1, transaction.transactionId);
    bindText(statement, 2, TransactionEngine::statusToString(transaction.status));
    sqlite3_bind_int64(statement, 3, now);

    const bool eventSuccess = sqlite3_step(statement) == SQLITE_DONE;
    sqlite3_finalize(statement);

    if (!eventSuccess || !commitTransaction()) return rollback();
    lastSaveResult = TransactionSaveResult::NEW_TRANSACTION;
    return lastSaveResult;
}

bool TransactionRepository::updateTransactionStatus(const Transaction& transaction)
{
    return updateTransactionStatusImpl(transaction, nullptr);
}

bool TransactionRepository::updateTransactionStatus(
    const Transaction& transaction,
    TransactionStatus expectedStatus)
{
    return updateTransactionStatusImpl(transaction, &expectedStatus);
}

ReplayAwareTransactionResult TransactionRepository::registerPaymentRequest(
    const Transaction& candidate, const RequestReplayData& request)
{
    std::lock_guard<std::mutex> lock(mutex_);
    ReplayAwareTransactionResult result;
    lastError_.clear();
    if (!initialized || !database || request.requestId.empty() || request.canonicalRequestHash.empty() ||
        request.paymentFingerprint.empty() || request.requestTimestamp < 0 || request.requestExpiry <= request.requestTimestamp)
    { lastError_ = "Invalid replay registration or repository is not ready"; return result; }
    if (!executeSql("BEGIN IMMEDIATE;")) return result;
    auto fail = [this, &result]() { if (lastError_.empty()) lastError_ = sqlite3_errmsg(database); rollbackTransaction(); result.code = ReplayRegistrationCode::DATABASE_ERROR; return result; };

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(database, "SELECT canonical_request_hash,payment_fingerprint,transaction_id,replay_state,COALESCE(response_status,''),COALESCE(adapter_result_code,''),COALESCE(adapter_message,''),COALESCE(unknown_reason,''),COALESCE(terminal_id,''),COALESCE(simulation_contract,'') FROM replay_records WHERE request_id=?", -1, &stmt, nullptr) != SQLITE_OK) return fail();
    bindText(stmt, 1, request.requestId);
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW)
    {
        const bool identical = sqliteText(stmt, 0) == request.canonicalRequestHash && sqliteText(stmt, 1) == request.paymentFingerprint;
        const std::string id = sqliteText(stmt, 2), state = sqliteText(stmt, 3);
        result.responseStatus = sqliteText(stmt, 4); result.adapterResultCode = sqliteText(stmt, 5);
        result.adapterMessage = sqliteText(stmt, 6); result.unknownReason = sqliteText(stmt, 7);
        result.terminalId = sqliteText(stmt, 8); result.simulationContract = sqliteText(stmt, 9);
        sqlite3_finalize(stmt);
        if (!identical) { rollbackTransaction(); result.code = ReplayRegistrationCode::REQUEST_ID_CONFLICT; return result; }
        if (!loadTransactionByIdUnlocked(id, result.transaction)) return fail();
        result.code = ReplayRegistrationCode::IDENTICAL_RETRY;
        result.replayState = state == "COMPLETED" ? ReplayState::COMPLETED : state == "FAILED" ? ReplayState::FAILED : state == "PROCESSING" ? ReplayState::PROCESSING : state == "RESERVED" ? ReplayState::RESERVED : ReplayState::UNKNOWN;
        if (!rollbackTransaction()) return fail();
        return result;
    }
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return fail();

    Transaction canonical;
    const bool hasReference = findTransactionByExternalReferenceUnlocked(candidate.externalReference, canonical);
    if (hasReference)
    {
        if (!samePayment(candidate, canonical)) { rollbackTransaction(); result.code = ReplayRegistrationCode::EXTERNAL_REFERENCE_CONFLICT; return result; }
        result.transaction = canonical;
        result.code = ReplayRegistrationCode::EXISTING_EXTERNAL_REFERENCE;
        result.replayState = canonical.status == TransactionStatus::FAILED ? ReplayState::FAILED : canonical.status == TransactionStatus::UNKNOWN ? ReplayState::UNKNOWN : (canonical.status == TransactionStatus::CREATED || canonical.status == TransactionStatus::SENT) ? ReplayState::PROCESSING : ReplayState::COMPLETED;
        result.responseStatus = TransactionEngine::statusToString(canonical.status);
    }
    else
    {
        result.transaction = candidate;
        result.transaction.status = TransactionStatus::CREATED;
        const char* insert = "INSERT INTO transactions(transaction_id,external_reference,amount_units,money_precision,currency,asset_type,asset_code,asset_network,contract_address,lifecycle_state,legacy_imported,created_at_unix,updated_at_unix) VALUES(?,?,?,?,?,?,?,?,?, 'CREATED',0,unixepoch(),unixepoch())";
        if (sqlite3_prepare_v2(database, insert, -1, &stmt, nullptr) != SQLITE_OK) return fail();
        bindText(stmt,1,candidate.transactionId); bindText(stmt,2,candidate.externalReference); sqlite3_bind_int64(stmt,3,candidate.money.units); sqlite3_bind_int(stmt,4,candidate.money.precision);
        if (candidate.asset.isFiat()) sqlite3_bind_text(stmt,5,candidate.currency.c_str(),-1,SQLITE_TRANSIENT); else sqlite3_bind_null(stmt,5);
        bindText(stmt,6,candidate.asset.isFiat()?"FIAT":"DIGITAL_ASSET"); bindText(stmt,7,candidate.asset.code);
        if (candidate.asset.isFiat()) { sqlite3_bind_null(stmt,8); sqlite3_bind_null(stmt,9); } else { bindText(stmt,8,candidate.asset.network); bindText(stmt,9,candidate.asset.contractAddress); }
        rc=sqlite3_step(stmt); sqlite3_finalize(stmt); if(rc!=SQLITE_DONE) return fail();
        result.code = ReplayRegistrationCode::NEW_REQUEST; result.replayState = ReplayState::PROCESSING; result.shouldProcess = true;
    }
    const std::int64_t now = unixNow();
    const std::string replayState = result.shouldProcess ? "PROCESSING" : result.replayState == ReplayState::FAILED ? "FAILED" : result.replayState == ReplayState::UNKNOWN ? "UNKNOWN" : result.replayState == ReplayState::PROCESSING ? "PROCESSING" : "COMPLETED";
    if (sqlite3_prepare_v2(database,"INSERT INTO replay_records(request_id,transaction_id,external_reference,canonical_request_hash,payment_fingerprint,request_timestamp_unix,request_expiry_unix,replay_state,processing_started_at_unix,response_status,created_at_unix,updated_at_unix) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)",-1,&stmt,nullptr)!=SQLITE_OK) return fail();
    bindText(stmt,1,request.requestId); bindText(stmt,2,result.transaction.transactionId); bindText(stmt,3,result.transaction.externalReference); bindText(stmt,4,request.canonicalRequestHash); bindText(stmt,5,request.paymentFingerprint); sqlite3_bind_int64(stmt,6,request.requestTimestamp); sqlite3_bind_int64(stmt,7,request.requestExpiry); bindText(stmt,8,replayState); if(result.shouldProcess) sqlite3_bind_int64(stmt,9,now); else sqlite3_bind_null(stmt,9); bindText(stmt,10,result.responseStatus); sqlite3_bind_int64(stmt,11,now); sqlite3_bind_int64(stmt,12,now);
    rc=sqlite3_step(stmt); sqlite3_finalize(stmt); if(rc!=SQLITE_DONE) return fail();
    if (result.shouldProcess) {
        if (sqlite3_prepare_v2(database,"INSERT INTO transaction_lifecycle_events(transaction_id,replay_request_id,from_status,to_status,event_type,event_time_unix) VALUES(?,?,NULL,'CREATED','RESERVED',?)",-1,&stmt,nullptr)!=SQLITE_OK) return fail();
        bindText(stmt,1,result.transaction.transactionId); bindText(stmt,2,request.requestId); sqlite3_bind_int64(stmt,3,now); rc=sqlite3_step(stmt); sqlite3_finalize(stmt); if(rc!=SQLITE_DONE) return fail();
    }
    if (!commitTransaction()) return fail();
    return result;
}

bool TransactionRepository::bindSimulatedTerminal(const std::string& requestId,
    const std::string& terminalId, const std::string& contractVersion)
{
    std::lock_guard<std::mutex> lock(mutex_);
    lastError_.clear();
    if (!initialized || !database || requestId.empty() || terminalId.empty() || contractVersion.empty())
    { lastError_ = "Invalid simulated terminal assignment"; return false; }
    sqlite3_stmt* stmt = nullptr;
    const char* sql = R"SQL(
UPDATE replay_records SET terminal_id=?, simulation_contract=?, updated_at_unix=unixepoch()
WHERE request_id=? AND replay_state='PROCESSING'
  AND (terminal_id IS NULL OR terminal_id=?)
  AND (simulation_contract IS NULL OR simulation_contract=?);
)SQL";
    if (sqlite3_prepare_v2(database, sql, -1, &stmt, nullptr) != SQLITE_OK)
    { lastError_ = sqlite3_errmsg(database); return false; }
    bindText(stmt, 1, terminalId); bindText(stmt, 2, contractVersion);
    bindText(stmt, 3, requestId); bindText(stmt, 4, terminalId); bindText(stmt, 5, contractVersion);
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(database) == 1;
    sqlite3_finalize(stmt);
    if (!ok) lastError_ = "Could not persist simulated terminal assignment";
    return ok;
}

bool TransactionRepository::recoverExpiredProcessingRequests()
{
    std::lock_guard<std::mutex> lock(mutex_);
    lastError_.clear();
    if (!initialized || !database) { lastError_ = "Repository is not initialized"; return false; }
    if (!executeSql("BEGIN IMMEDIATE;")) return false;
    struct Pending { std::string requestId, transactionId, status; };
    std::vector<Pending> pending;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(database,
        "SELECT request_id,transaction_id,(SELECT lifecycle_state FROM transactions t WHERE t.transaction_id=replay_records.transaction_id) FROM replay_records WHERE replay_state='PROCESSING' AND request_expiry_unix < unixepoch()",
        -1, &stmt, nullptr) != SQLITE_OK)
    { lastError_ = sqlite3_errmsg(database); rollbackTransaction(); return false; }
    int rc = SQLITE_ROW;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
        pending.push_back({sqliteText(stmt, 0), sqliteText(stmt, 1), sqliteText(stmt, 2)});
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) { lastError_ = sqlite3_errmsg(database); rollbackTransaction(); return false; }

    const std::int64_t now = unixNow();
    for (const auto& item : pending)
    {
        const bool uncertain = item.status == "SENT" || item.status == "RECEIVED";
        if (!uncertain && item.status != "CREATED") continue;
        const std::string target = uncertain ? "UNKNOWN" : "FAILED";
        const std::string state = uncertain ? "UNKNOWN" : "FAILED";
        const std::string reason = item.status == "RECEIVED"
            ? "Restart recovery: response was received but final outcome was not persisted; resend disabled"
            : uncertain
            ? "Restart recovery: terminal operation may have reached the terminal; resend disabled"
            : "Restart recovery: request expired before terminal send";
        if (sqlite3_prepare_v2(database,
            "UPDATE transactions SET lifecycle_state=?,updated_at_unix=max(updated_at_unix,?) WHERE transaction_id=? AND lifecycle_state=?",
            -1, &stmt, nullptr) != SQLITE_OK)
        { lastError_ = sqlite3_errmsg(database); rollbackTransaction(); return false; }
        bindText(stmt, 1, target); sqlite3_bind_int64(stmt, 2, now); bindText(stmt, 3, item.transactionId); bindText(stmt, 4, item.status);
        const bool txUpdated = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(database) == 1;
        sqlite3_finalize(stmt);
        if (!txUpdated) { lastError_ = "Restart recovery transaction state conflict"; rollbackTransaction(); return false; }
        if (sqlite3_prepare_v2(database,
            "UPDATE replay_records SET replay_state=?,response_status=?,adapter_result_code='RESTART_RECOVERY',adapter_message=?,unknown_reason=?,terminal_at_unix=?,updated_at_unix=max(updated_at_unix,?) WHERE request_id=? AND replay_state='PROCESSING'",
            -1, &stmt, nullptr) != SQLITE_OK)
        { lastError_ = sqlite3_errmsg(database); rollbackTransaction(); return false; }
        bindText(stmt, 1, state); bindText(stmt, 2, target); bindText(stmt, 3, reason);
        bindText(stmt, 4, uncertain ? reason : std::string{}); sqlite3_bind_int64(stmt, 5, now);
        sqlite3_bind_int64(stmt, 6, now); bindText(stmt, 7, item.requestId);
        const bool replayUpdated = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(database) == 1;
        sqlite3_finalize(stmt);
        if (!replayUpdated) { lastError_ = "Restart recovery replay state conflict"; rollbackTransaction(); return false; }
        if (sqlite3_prepare_v2(database,
            "INSERT INTO transaction_lifecycle_events(transaction_id,replay_request_id,from_status,to_status,event_type,event_time_unix,reason) VALUES(?,?,?,?,?,?,?)",
            -1, &stmt, nullptr) != SQLITE_OK)
        { lastError_ = sqlite3_errmsg(database); rollbackTransaction(); return false; }
        bindText(stmt, 1, item.transactionId); bindText(stmt, 2, item.requestId);
        bindText(stmt, 3, item.status); bindText(stmt, 4, target);
        bindText(stmt, 5, uncertain ? "UNKNOWN" : "LIFECYCLE"); sqlite3_bind_int64(stmt, 6, now); bindText(stmt, 7, reason);
        const bool eventAdded = sqlite3_step(stmt) == SQLITE_DONE;
        sqlite3_finalize(stmt);
        if (!eventAdded) { lastError_ = sqlite3_errmsg(database); rollbackTransaction(); return false; }
    }
    if (!commitTransaction()) { rollbackTransaction(); return false; }
    return true;
}

bool TransactionRepository::updateTransactionStatusForRequest(const Transaction& t, TransactionStatus expected, const std::string& requestId, ReplayState state, const std::string& responseStatus, const std::string& code, const std::string& message, const std::string& reason)
{
    return updateTransactionStatusImpl(t, &expected, &requestId, state, responseStatus, code, message, reason);
}

bool TransactionRepository::updateTransactionStatusImpl(
    const Transaction& transaction,
    const TransactionStatus* expectedStatus,
    const std::string* replayRequestId,
    ReplayState replayState,
    const std::string& responseStatus,
    const std::string& adapterResultCode,
    const std::string& adapterMessage,
    const std::string& unknownReason)
{
    std::lock_guard<std::mutex> lock(mutex_);
    lastError_.clear();
    if (!initialized || !database || transaction.transactionId.empty())
        return false;

    if (!executeSql("BEGIN IMMEDIATE;")) return false;
    auto fail = [this]() {
        if (database && lastError_.empty())
            lastError_ = sqlite3_errmsg(database);
        rollbackTransaction();
        return false;
    };

    sqlite3_stmt* statement = nullptr;

    // First, retrieve the current status of the transaction.
    const char* selectSql = "SELECT lifecycle_state FROM transactions WHERE transaction_id = ?;";
    if (sqlite3_prepare_v2(database, selectSql, -1, &statement, nullptr) != SQLITE_OK)
        return fail();

    bindText(statement, 1, transaction.transactionId);

    TransactionStatus currentStatus = TransactionStatus::UNKNOWN;
    if (sqlite3_step(statement) == SQLITE_ROW)
    {
        const std::string currentStatusStr = sqliteText(statement, 0);
        if (currentStatusStr == "CREATED") currentStatus = TransactionStatus::CREATED;
        else if (currentStatusStr == "SENT") currentStatus = TransactionStatus::SENT;
        else if (currentStatusStr == "RECEIVED") currentStatus = TransactionStatus::RECEIVED;
        else if (currentStatusStr == "APPROVED") currentStatus = TransactionStatus::APPROVED;
        else if (currentStatusStr == "DECLINED") currentStatus = TransactionStatus::DECLINED;
        else if (currentStatusStr == "FAILED") currentStatus = TransactionStatus::FAILED;
        else if (currentStatusStr == "UNKNOWN") currentStatus = TransactionStatus::UNKNOWN;
        else
        {
            // Invalid current status, handle error (e.g., log it, throw an exception, etc.)
            sqlite3_finalize(statement);
            return fail();
        }
    }
    else
    {
        // Transaction not found, handle error (e.g., log it, throw an exception, etc.)
        sqlite3_finalize(statement);
        return fail();
    }

    sqlite3_finalize(statement);

    if (expectedStatus && currentStatus != *expectedStatus)
    {
        lastError_ = "Transaction lifecycle state changed in SQLite";
        return fail();
    }

    // Validate the status transition.
    if (!TransactionEngine::isValidTransition(currentStatus, transaction.status))
    {
        lastError_ = "Invalid transaction lifecycle transition";
        return fail();
    }

    statement = nullptr;
    const char* sql = "UPDATE transactions SET lifecycle_state = ?, updated_at_unix = ? WHERE transaction_id = ?;";

    if (sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) != SQLITE_OK)
        return fail();

    const std::int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    bindText(statement, 1, TransactionEngine::statusToString(transaction.status));
    sqlite3_bind_int64(statement, 2, now);
    bindText(statement, 3, transaction.transactionId);

    const bool success = sqlite3_step(statement) == SQLITE_DONE;
    const bool changed = success && sqlite3_changes(database) == 1;
    sqlite3_finalize(statement);

    if (!changed)
        return fail();

    statement = nullptr;
    const char* eventSql = R"SQL(
INSERT INTO transaction_lifecycle_events (
    transaction_id, replay_request_id, from_status, to_status,
    event_type, event_time_unix, reason
) VALUES (?, ?, ?, ?, 'LIFECYCLE', ?, NULL);
)SQL";

    if (sqlite3_prepare_v2(database, eventSql, -1, &statement, nullptr) != SQLITE_OK)
        return fail();

    bindText(statement, 1, transaction.transactionId);
    if (replayRequestId) bindText(statement, 2, *replayRequestId); else sqlite3_bind_null(statement, 2);
    bindText(statement, 3, TransactionEngine::statusToString(currentStatus));
    bindText(statement, 4, TransactionEngine::statusToString(transaction.status));
    sqlite3_bind_int64(statement, 5, now);

    const bool eventSuccess = sqlite3_step(statement) == SQLITE_DONE;
    sqlite3_finalize(statement);

    if (!eventSuccess) return fail();
    if (replayRequestId)
    {
        const char* replaySql = "UPDATE replay_records SET replay_state=?,response_status=?,adapter_result_code=?,adapter_message=?,unknown_reason=?,terminal_at_unix=?,updated_at_unix=? WHERE request_id=? AND transaction_id=? AND replay_state='PROCESSING'";
        if (sqlite3_prepare_v2(database, replaySql, -1, &statement, nullptr) != SQLITE_OK) return fail();
        const char* stateText = replayState == ReplayState::COMPLETED ? "COMPLETED" : replayState == ReplayState::FAILED ? "FAILED" : replayState == ReplayState::UNKNOWN ? "UNKNOWN" : replayState == ReplayState::RESERVED ? "RESERVED" : "PROCESSING";
        bindText(statement,1,stateText); bindText(statement,2,responseStatus); bindText(statement,3,adapterResultCode); bindText(statement,4,adapterMessage); bindText(statement,5,unknownReason);
        if (replayState == ReplayState::COMPLETED || replayState == ReplayState::FAILED || replayState == ReplayState::UNKNOWN) sqlite3_bind_int64(statement,6,now); else sqlite3_bind_null(statement,6);
        sqlite3_bind_int64(statement,7,now); bindText(statement,8,*replayRequestId); bindText(statement,9,transaction.transactionId);
        const bool updated = sqlite3_step(statement) == SQLITE_DONE && sqlite3_changes(database) == 1; sqlite3_finalize(statement); if (!updated) return fail();
    }
    if (!commitTransaction()) return fail();
    return true;
}

std::vector<Transaction> TransactionRepository::loadTransactions() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Transaction> transactions;
    lastError_.clear();
    if (!initialized || !database)
        return transactions;

    sqlite3_stmt* statement = nullptr;
    const char* sql = R"SQL(
SELECT transaction_id, external_reference, amount_units, money_precision,
       currency, asset_type, asset_code, asset_network, contract_address,
       lifecycle_state
FROM transactions ORDER BY transaction_id;
)SQL";

    if (sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) != SQLITE_OK)
    {
        lastError_ = sqlite3_errmsg(database);
        runtimeReadFailure_ = true;
        return transactions;
    }

    int stepResult = SQLITE_OK;
    while ((stepResult = sqlite3_step(statement)) == SQLITE_ROW)
    {
        Transaction transaction;
        transaction.transactionId = sqliteText(statement, 0);
        transaction.externalReference = sqliteText(statement, 1);
        transaction.amountMinor = sqlite3_column_int64(statement, 2);
        transaction.money.units = transaction.amountMinor;
        transaction.money.precision = static_cast<std::uint8_t>(sqlite3_column_int(statement, 3));

        const std::string assetType = sqliteText(statement, 5);
        transaction.asset.code = sqliteText(statement, 6);

        if (assetType == "FIAT")
        {
            transaction.asset.type = paybridge::assets::AssetType::FIAT;
            transaction.currency = sqliteText(statement, 4);
        }
        else if (assetType == "DIGITAL_ASSET")
        {
            transaction.asset.type = paybridge::assets::AssetType::DIGITAL_ASSET;
            transaction.asset.network = sqliteText(statement, 7);
            transaction.asset.contractAddress = sqliteText(statement, 8);
        }
        else
        {
            lastError_ = "Unrecognized asset type in SQLite transaction data";
            runtimeReadFailure_ = true;
            sqlite3_finalize(statement);
            return {};
        }

        transaction.asset.decimalPrecision = transaction.money.precision;

        const std::string state = sqliteText(statement, 9);
        if (state == "CREATED") transaction.status = TransactionStatus::CREATED;
        else if (state == "SENT") transaction.status = TransactionStatus::SENT;
        else if (state == "RECEIVED") transaction.status = TransactionStatus::RECEIVED;
        else if (state == "APPROVED") transaction.status = TransactionStatus::APPROVED;
        else if (state == "DECLINED") transaction.status = TransactionStatus::DECLINED;
        else if (state == "FAILED") transaction.status = TransactionStatus::FAILED;
        else if (state == "UNKNOWN") transaction.status = TransactionStatus::UNKNOWN;
        else if (state == "INVALID") transaction.status = TransactionStatus::INVALID;
        else
        {
            sqlite3_finalize(statement);
            lastError_ = "Unrecognized lifecycle state in SQLite transaction data";
            runtimeReadFailure_ = true;
            transactions.clear();
            return transactions;
        }

        transactions.push_back(transaction);
    }

    sqlite3_finalize(statement);
    if (stepResult != SQLITE_DONE)
    {
        lastError_ = sqlite3_errmsg(database);
        runtimeReadFailure_ = true;
        return {};
    }
    return transactions;
}

TransactionPage TransactionRepository::listTransactions(
    std::size_t pageSize,
    std::optional<TransactionStatus> statusFilter,
    std::optional<TransactionCursor> cursor) const
{
    TransactionPage page;
    if (pageSize == 0 ||
        (cursor && (cursor->createdAtUnix < 0 || cursor->transactionId.empty() ||
                    cursor->transactionId.size() > MAX_CURSOR_TRANSACTION_ID_LENGTH)) ||
        (statusFilter && statusDatabaseText(*statusFilter) == nullptr))
    {
        page.validRequest = false;
        return page;
    }
    pageSize = std::min(pageSize, MAX_TRANSACTION_PAGE_SIZE);

    std::lock_guard<std::mutex> lock(mutex_);
    lastError_.clear();
    if (!initialized || !database)
    {
        lastError_ = "SQLite repository is not initialized";
        return page;
    }

    const char* baseSql = R"SQL(
SELECT t.transaction_id, t.amount_units, t.money_precision, COALESCE(t.currency,''),
       t.asset_type, t.asset_code, COALESCE(t.asset_network,''),
       COALESCE(t.contract_address,''), t.lifecycle_state,
       t.created_at_unix, t.updated_at_unix,
       COALESCE((SELECT r.terminal_id FROM replay_records r
                 WHERE r.transaction_id=t.transaction_id
                   AND r.terminal_id IS NOT NULL AND r.terminal_id <> ''
                 ORDER BY r.updated_at_unix DESC, r.request_id DESC LIMIT 1),'')
FROM transactions t
)SQL";
    std::string sql = baseSql;
    if (statusFilter) sql += " WHERE t.lifecycle_state=?";
    if (cursor)
        sql += statusFilter
            ? " AND (t.created_at_unix < ? OR (t.created_at_unix = ? AND t.transaction_id < ?))"
            : " WHERE (t.created_at_unix < ? OR (t.created_at_unix = ? AND t.transaction_id < ?))";
    sql += " ORDER BY t.created_at_unix DESC, t.transaction_id DESC LIMIT ?";

    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK)
    {
        lastError_ = sqlite3_errmsg(database);
        runtimeReadFailure_ = true;
        return page;
    }
    int parameter = 1;
    if (statusFilter)
        bindText(statement, parameter++, statusDatabaseText(*statusFilter));
    if (cursor)
    {
        sqlite3_bind_int64(statement, parameter++, cursor->createdAtUnix);
        sqlite3_bind_int64(statement, parameter++, cursor->createdAtUnix);
        bindText(statement, parameter++, cursor->transactionId);
    }
    sqlite3_bind_int64(statement, parameter++, static_cast<sqlite3_int64>(pageSize + 1));

    int result = SQLITE_OK;
    while ((result = sqlite3_step(statement)) == SQLITE_ROW)
    {
        if (page.transactions.size() == pageSize)
        {
            page.hasMore = true;
            break;
        }
        TransactionReadRecord record;
        if (!readTransactionRecord(statement, record))
        {
            lastError_ = "Unrecognized transaction metadata in SQLite";
            runtimeReadFailure_ = true;
            page.transactions.clear();
            sqlite3_finalize(statement);
            return page;
        }
        page.transactions.push_back(std::move(record));
    }
    sqlite3_finalize(statement);
    if (result != SQLITE_DONE && result != SQLITE_ROW)
    {
        lastError_ = sqlite3_errmsg(database);
        runtimeReadFailure_ = true;
        page.transactions.clear();
        return page;
    }
    page.success = true;
    if (page.hasMore && !page.transactions.empty())
    {
        const auto& last = page.transactions.back();
        page.nextCursor = TransactionCursor{last.createdAtUnix, last.transactionId};
    }
    return page;
}

std::optional<TransactionReadRecord> TransactionRepository::findTransactionById(
    const std::string& transactionId) const
{
    if (transactionId.empty() || transactionId.size() > MAX_CURSOR_TRANSACTION_ID_LENGTH)
        return std::nullopt;
    std::lock_guard<std::mutex> lock(mutex_);
    lastError_.clear();
    if (!initialized || !database)
    {
        lastError_ = "SQLite repository is not initialized";
        return std::nullopt;
    }
    const char* sql = R"SQL(
SELECT t.transaction_id, t.amount_units, t.money_precision, COALESCE(t.currency,''),
       t.asset_type, t.asset_code, COALESCE(t.asset_network,''),
       COALESCE(t.contract_address,''), t.lifecycle_state,
       t.created_at_unix, t.updated_at_unix,
       COALESCE((SELECT r.terminal_id FROM replay_records r
                 WHERE r.transaction_id=t.transaction_id
                   AND r.terminal_id IS NOT NULL AND r.terminal_id <> ''
                 ORDER BY r.updated_at_unix DESC, r.request_id DESC LIMIT 1),'')
FROM transactions t WHERE t.transaction_id=?
)SQL";
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) != SQLITE_OK)
    {
        lastError_ = sqlite3_errmsg(database);
        runtimeReadFailure_ = true;
        return std::nullopt;
    }
    bindText(statement, 1, transactionId);
    const int result = sqlite3_step(statement);
    if (result == SQLITE_DONE)
    {
        sqlite3_finalize(statement);
        return std::nullopt;
    }
    if (result != SQLITE_ROW)
    {
        lastError_ = sqlite3_errmsg(database);
        runtimeReadFailure_ = true;
        sqlite3_finalize(statement);
        return std::nullopt;
    }
    TransactionReadRecord record;
    const bool valid = readTransactionRecord(statement, record);
    sqlite3_finalize(statement);
    if (!valid)
    {
        lastError_ = "Unrecognized transaction metadata in SQLite";
        runtimeReadFailure_ = true;
        return std::nullopt;
    }
    return record;
}

bool TransactionRepository::loadTransactionByIdUnlocked(const std::string& id, Transaction& transaction) const
{
    sqlite3_stmt* stmt=nullptr;
    const char* sql="SELECT transaction_id,external_reference,amount_units,money_precision,currency,asset_type,asset_code,asset_network,contract_address,lifecycle_state FROM transactions WHERE transaction_id=?";
    if(sqlite3_prepare_v2(database,sql,-1,&stmt,nullptr)!=SQLITE_OK) return false;
    bindText(stmt,1,id); const int rc=sqlite3_step(stmt);
    if(rc==SQLITE_ROW) {
        transaction.transactionId=sqliteText(stmt,0); transaction.externalReference=sqliteText(stmt,1); transaction.amountMinor=sqlite3_column_int64(stmt,2); transaction.money.units=transaction.amountMinor; transaction.money.precision=static_cast<std::uint8_t>(sqlite3_column_int(stmt,3));
        const std::string type=sqliteText(stmt,5); transaction.asset.type=type=="FIAT"?paybridge::assets::AssetType::FIAT:paybridge::assets::AssetType::DIGITAL_ASSET; transaction.asset.code=sqliteText(stmt,6); transaction.currency=sqliteText(stmt,4); transaction.asset.network=sqliteText(stmt,7); transaction.asset.contractAddress=sqliteText(stmt,8); transaction.asset.decimalPrecision=transaction.money.precision;
        const std::string s=sqliteText(stmt,9); transaction.status=s=="CREATED"?TransactionStatus::CREATED:s=="SENT"?TransactionStatus::SENT:s=="RECEIVED"?TransactionStatus::RECEIVED:s=="APPROVED"?TransactionStatus::APPROVED:s=="DECLINED"?TransactionStatus::DECLINED:s=="FAILED"?TransactionStatus::FAILED:s=="INVALID"?TransactionStatus::INVALID:TransactionStatus::UNKNOWN;
    }
    sqlite3_finalize(stmt); return rc==SQLITE_ROW;
}

bool TransactionRepository::findTransactionByExternalReferenceUnlocked(const std::string& reference, Transaction& transaction) const
{
    sqlite3_stmt* stmt=nullptr;
    if(sqlite3_prepare_v2(database,"SELECT transaction_id FROM transactions WHERE external_reference=?",-1,&stmt,nullptr)!=SQLITE_OK) return false;
    bindText(stmt,1,reference); const int rc=sqlite3_step(stmt); std::string id; if(rc==SQLITE_ROW) id=sqliteText(stmt,0); sqlite3_finalize(stmt);
    return rc==SQLITE_ROW && loadTransactionByIdUnlocked(id,transaction);
}

bool TransactionRepository::findByExternalReference(
    const std::string& externalReference,
    Transaction& transaction) const
{
    const auto transactions = loadTransactions();
    for (const Transaction& candidate : transactions)
    {
        if (candidate.externalReference == externalReference)
        {
            transaction = candidate;
            return true;
        }
    }
    return false;
}

LegacyMigrationResult TransactionRepository::importLegacyCsv(
    const std::string& sourceName,
    const std::string& sourceHash,
    const std::vector<LegacyCsvRecord>& records)
{
    std::lock_guard<std::mutex> lock(mutex_);
    LegacyMigrationResult report;
    report.sourceRowCount = records.size();
    if (!initialized || !database || sourceName.empty() || sourceHash.empty())
    {
        report.diagnostics.push_back("SQLite repository or migration source is not ready");
        return report;
    }

    struct History
    {
        std::vector<const LegacyCsvRecord*> rows;
        Transaction finalTransaction;
    };
    std::unordered_map<std::string, History> histories;
    std::unordered_map<std::string, std::string> transactionReferences;
    std::unordered_map<std::size_t, bool> rowNumbers;

    for (const auto& row : records)
    {
        if (row.rowNumber == 0 || !rowNumbers.emplace(row.rowNumber, true).second)
        {
            ++report.conflictCount;
            report.diagnostics.push_back("CSV row number is missing or duplicated");
            continue;
        }
        if (!row.valid)
        {
            ++report.conflictCount;
            report.diagnostics.push_back("CSV row " + std::to_string(row.rowNumber) +
                ": " + (row.diagnostic.empty() ? "malformed transaction row" : row.diagnostic));
            continue;
        }

        const Transaction& transaction = row.transaction;
        if (transaction.transactionId.empty() || transaction.externalReference.empty() ||
            !transaction.asset.isComplete() || transaction.money.units <= 0 ||
            transaction.money.precision != transaction.asset.decimalPrecision ||
            row.rawRecordHash.empty())
        {
            ++report.conflictCount;
            report.diagnostics.push_back("CSV row " + std::to_string(row.rowNumber) +
                ": transaction identity or exact-money fields are incomplete");
            continue;
        }

        const auto id = transactionReferences.emplace(
            transaction.transactionId, transaction.externalReference);
        if (!id.second && id.first->second != transaction.externalReference)
        {
            ++report.conflictCount;
            report.diagnostics.push_back("CSV row " + std::to_string(row.rowNumber) +
                ": one transaction ID is associated with multiple external references");
            continue;
        }

        auto& history = histories[transaction.externalReference];
        if (!history.rows.empty())
        {
            const Transaction& first = history.rows.front()->transaction;
            if (first.transactionId != transaction.transactionId ||
                !samePayment(first, transaction))
            {
                ++report.conflictCount;
                report.diagnostics.push_back("CSV row " + std::to_string(row.rowNumber) +
                    ": external reference has conflicting transaction identity or payment data");
                continue;
            }
        }
        history.rows.push_back(&row);
        history.finalTransaction = transaction;
    }

    for (const auto& item : histories)
    {
        const auto& rows = item.second.rows;
        if (rows.empty())
            continue;
        if (rows.front()->transaction.status != TransactionStatus::CREATED)
        {
            ++report.conflictCount;
            report.diagnostics.push_back("External reference " + item.first +
                ": history does not begin at CREATED");
            continue;
        }
        TransactionStatus previous = rows.front()->transaction.status;
        for (std::size_t index = 1; index < rows.size(); ++index)
        {
            const TransactionStatus next = rows[index]->transaction.status;
            if (next == previous) // Duplicate snapshot; retain its raw source row only.
                continue;
            if (!TransactionEngine::isValidTransition(previous, next))
            {
                ++report.conflictCount;
                report.diagnostics.push_back("CSV row " +
                    std::to_string(rows[index]->rowNumber) +
                    ": invalid lifecycle transition from " +
                    TransactionEngine::statusToString(previous) + " to " +
                    TransactionEngine::statusToString(next));
                break;
            }
            previous = next;
        }
    }

    const std::string sourceKey = sourceName + "#" + sourceHash;
    const std::string batchId = digestHex(sourceName + "\n" + sourceHash);
    const std::int64_t now = unixNow();
    if (!executeSql("BEGIN IMMEDIATE;"))
    {
        report.diagnostics.push_back(lastError_);
        return report;
    }
    auto rollback = [this, &report]() {
        lastError_ = sqlite3_errmsg(database);
        rollbackTransaction();
        report.diagnostics.push_back(lastError_);
        return report;
    };

    // A completed source is immutable after cutover. An identical source is
    // idempotent; changed bytes must be reconciled explicitly before startup.
    sqlite3_stmt* completed = nullptr;
    if (sqlite3_prepare_v2(database,
            "SELECT source_hash FROM migration_batches WHERE source_name=? AND status='COMPLETE' ORDER BY started_at_unix DESC LIMIT 1",
            -1, &completed, nullptr) != SQLITE_OK)
        return rollback();
    bindText(completed, 1, sourceName);
    const int completedRc = sqlite3_step(completed);
    const std::string completedHash = completedRc == SQLITE_ROW ? sqliteText(completed, 0) : std::string{};
    sqlite3_finalize(completed);
    if (completedRc == SQLITE_ROW)
    {
        if (completedHash == sourceHash)
        {
            bool reconciled = true;
            const char* verifySql = R"SQL(
SELECT transaction_id, amount_units, money_precision, COALESCE(currency,''),
       asset_type, asset_code, COALESCE(asset_network,''),
       COALESCE(contract_address,''), lifecycle_state
FROM transactions WHERE external_reference=?
)SQL";
            for (const auto& item : histories)
            {
                const Transaction& tx = item.second.finalTransaction;
                sqlite3_stmt* verify = nullptr;
                if (sqlite3_prepare_v2(database, verifySql, -1, &verify, nullptr) != SQLITE_OK)
                {
                    reconciled = false;
                    report.diagnostics.push_back(sqlite3_errmsg(database));
                    break;
                }
                bindText(verify, 1, tx.externalReference);
                const int verifyRc = sqlite3_step(verify);
                const bool rowMatches = verifyRc == SQLITE_ROW &&
                    sqliteText(verify, 0) == tx.transactionId &&
                    sqlite3_column_int64(verify, 1) == tx.money.units &&
                    sqlite3_column_int(verify, 2) == tx.money.precision &&
                    sqliteText(verify, 3) == (tx.asset.isFiat() ? tx.currency : "") &&
                    sqliteText(verify, 4) == (tx.asset.isFiat() ? "FIAT" : "DIGITAL_ASSET") &&
                    sqliteText(verify, 5) == tx.asset.code &&
                    sqliteText(verify, 6) == tx.asset.network &&
                    sqliteText(verify, 7) == tx.asset.contractAddress &&
                    statusTextCanAdvanceTo(tx.status, sqliteText(verify, 8));
                sqlite3_finalize(verify);
                if (!rowMatches)
                {
                    reconciled = false;
                    report.diagnostics.push_back("Previously migrated CSV source no longer reconciles with SQLite for external reference " + tx.externalReference);
                    break;
                }
                ++report.reconciledRowCount;
            }
            if (!reconciled)
            {
                rollbackTransaction();
                report.conflictCount = 1;
                return report;
            }
            if (!rollbackTransaction())
                return rollback();
            report.success = true;
            report.alreadyMigrated = true;
            report.diagnostics.push_back("CSV source was already migrated successfully");
            return report;
        }
        ++report.conflictCount;
        report.diagnostics.push_back("CSV source changed after a completed migration; refusing automatic re-import");
    }

    sqlite3_stmt* batch = nullptr;
    const char* upsertBatch = R"SQL(
INSERT INTO migration_batches(
    batch_id, source_name, source_version, source_row_count,
    imported_row_count, reconciled_row_count, conflict_count, status,
    source_hash, started_at_unix, completed_at_unix
) VALUES (?, ?, 'csv-v1', ?, 0, 0, ?, 'RUNNING', ?, ?, NULL)
ON CONFLICT(batch_id) DO UPDATE SET
    source_row_count=excluded.source_row_count,
    imported_row_count=0,
    reconciled_row_count=0,
    conflict_count=excluded.conflict_count,
    status='RUNNING',
    source_hash=excluded.source_hash,
    started_at_unix=excluded.started_at_unix,
    completed_at_unix=NULL
)SQL";
    if (sqlite3_prepare_v2(database, upsertBatch, -1, &batch, nullptr) != SQLITE_OK)
        return rollback();
    bindText(batch, 1, batchId);
    bindText(batch, 2, sourceName);
    sqlite3_bind_int64(batch, 3, static_cast<sqlite3_int64>(report.sourceRowCount));
    sqlite3_bind_int64(batch, 4, static_cast<sqlite3_int64>(report.conflictCount));
    bindText(batch, 5, sourceHash);
    sqlite3_bind_int64(batch, 6, now);
    const bool batchOk = sqlite3_step(batch) == SQLITE_DONE;
    sqlite3_finalize(batch);
    if (!batchOk)
        return rollback();

    sqlite3_stmt* removeOldRows = nullptr;
    if (sqlite3_prepare_v2(database,
            "DELETE FROM migration_legacy_rows WHERE source_name=?", -1,
            &removeOldRows, nullptr) != SQLITE_OK)
        return rollback();
    bindText(removeOldRows, 1, sourceKey);
    const bool deleteOk = sqlite3_step(removeOldRows) == SQLITE_DONE;
    sqlite3_finalize(removeOldRows);
    if (!deleteOk)
        return rollback();

    const char* legacySql = R"SQL(
INSERT INTO migration_legacy_rows(
    source_name, source_row_number, transaction_id, external_reference,
    amount_units, currency, money_precision, asset_type, asset_code,
    asset_network, contract_address, lifecycle_state, raw_record, raw_record_hash
) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
)SQL";
    for (const auto& row : records)
    {
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(database, legacySql, -1, &statement, nullptr) != SQLITE_OK)
            return rollback();
        bindText(statement, 1, sourceKey);
        sqlite3_bind_int64(statement, 2, static_cast<sqlite3_int64>(row.rowNumber));
        if (row.valid)
        {
            const auto& tx = row.transaction;
            bindText(statement, 3, tx.transactionId);
            bindText(statement, 4, tx.externalReference);
            sqlite3_bind_int64(statement, 5, tx.money.units);
            if (tx.asset.isFiat()) bindText(statement, 6, tx.currency);
            else sqlite3_bind_null(statement, 6);
            sqlite3_bind_int(statement, 7, tx.money.precision);
            bindText(statement, 8, tx.asset.isFiat() ? "FIAT" : "DIGITAL_ASSET");
            bindText(statement, 9, tx.asset.code);
            if (tx.asset.network.empty()) sqlite3_bind_null(statement, 10);
            else bindText(statement, 10, tx.asset.network);
            if (tx.asset.contractAddress.empty()) sqlite3_bind_null(statement, 11);
            else bindText(statement, 11, tx.asset.contractAddress);
            bindText(statement, 12, TransactionEngine::statusToString(tx.status));
        }
        else
        {
            for (int column = 3; column <= 12; ++column)
                sqlite3_bind_null(statement, column);
        }
        bindText(statement, 13, row.rawRecord);
        bindText(statement, 14, row.rawRecordHash);
        const bool rowOk = sqlite3_step(statement) == SQLITE_DONE;
        sqlite3_finalize(statement);
        if (!rowOk)
            return rollback();
    }

    auto failBatch = [this, &report, now, &batchId]() {
        sqlite3_stmt* failed = nullptr;
        if (sqlite3_prepare_v2(database,
                "UPDATE migration_batches SET imported_row_count=?, reconciled_row_count=?, conflict_count=?, status='FAILED', completed_at_unix=? WHERE batch_id=?",
                -1, &failed, nullptr) != SQLITE_OK)
            return false;
        sqlite3_bind_int64(failed, 1, static_cast<sqlite3_int64>(report.importedRowCount));
        sqlite3_bind_int64(failed, 2, static_cast<sqlite3_int64>(report.reconciledRowCount));
        sqlite3_bind_int64(failed, 3, static_cast<sqlite3_int64>(report.conflictCount));
        sqlite3_bind_int64(failed, 4, now);
        bindText(failed, 5, batchId);
        const bool ok = sqlite3_step(failed) == SQLITE_DONE;
        sqlite3_finalize(failed);
        return ok && commitTransaction();
    };

    if (report.conflictCount != 0)
    {
        if (!failBatch())
            return rollback();
        return report;
    }

    const char* findSql = R"SQL(
SELECT transaction_id, external_reference, amount_units, money_precision,
       currency, asset_type, asset_code, asset_network, contract_address,
       lifecycle_state
FROM transactions WHERE external_reference=? OR transaction_id=?
)SQL";
    const char* insertTransactionSql = R"SQL(
INSERT INTO transactions(
 transaction_id, external_reference, amount_units, money_precision,
 currency, asset_type, asset_code, asset_network, contract_address,
 lifecycle_state, legacy_imported, created_at_unix, updated_at_unix
) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 1, ?, ?)
)SQL";
    const char* insertEventSql = R"SQL(
INSERT INTO transaction_lifecycle_events(
 transaction_id, replay_request_id, from_status, to_status,
 event_type, event_time_unix, reason
) VALUES(?, NULL, ?, ?, 'LEGACY_IMPORT', ?, ?)
)SQL";

    if (!executeSql("SAVEPOINT legacy_transaction_import;"))
        return rollback();

    for (const auto& entry : histories)
    {
        const auto& history = entry.second;
        if (history.rows.empty())
            continue;
        const Transaction& finalTx = history.finalTransaction;

        sqlite3_stmt* existing = nullptr;
        if (sqlite3_prepare_v2(database, findSql, -1, &existing, nullptr) != SQLITE_OK)
            return rollback();
        bindText(existing, 1, finalTx.externalReference);
        bindText(existing, 2, finalTx.transactionId);
        const int existingRc = sqlite3_step(existing);
        if (existingRc == SQLITE_ROW)
        {
            const bool identityMatches =
                sqliteText(existing, 0) == finalTx.transactionId &&
                sqliteText(existing, 1) == finalTx.externalReference &&
                sqlite3_column_int64(existing, 2) == finalTx.money.units &&
                sqlite3_column_int(existing, 3) == finalTx.money.precision &&
                sqliteText(existing, 4) == (finalTx.asset.isFiat() ? finalTx.currency : "") &&
                sqliteText(existing, 5) == (finalTx.asset.isFiat() ? "FIAT" : "DIGITAL_ASSET") &&
                sqliteText(existing, 6) == finalTx.asset.code &&
                sqliteText(existing, 7) == finalTx.asset.network &&
                sqliteText(existing, 8) == finalTx.asset.contractAddress &&
                statusTextCanAdvanceTo(finalTx.status, sqliteText(existing, 9));
            sqlite3_finalize(existing);
            if (!identityMatches)
            {
                ++report.conflictCount;
                report.diagnostics.push_back("SQLite already contains conflicting data for external reference " + finalTx.externalReference);
                continue;
            }
            ++report.reconciledRowCount;
            continue;
        }
        sqlite3_finalize(existing);
        if (existingRc != SQLITE_DONE)
            return rollback();

        sqlite3_stmt* insert = nullptr;
        if (sqlite3_prepare_v2(database, insertTransactionSql, -1, &insert, nullptr) != SQLITE_OK)
            return rollback();
        bindText(insert, 1, finalTx.transactionId);
        bindText(insert, 2, finalTx.externalReference);
        sqlite3_bind_int64(insert, 3, finalTx.money.units);
        sqlite3_bind_int(insert, 4, finalTx.money.precision);
        if (finalTx.asset.isFiat()) bindText(insert, 5, finalTx.currency);
        else sqlite3_bind_null(insert, 5);
        bindText(insert, 6, finalTx.asset.isFiat() ? "FIAT" : "DIGITAL_ASSET");
        bindText(insert, 7, finalTx.asset.code);
        if (finalTx.asset.network.empty()) sqlite3_bind_null(insert, 8);
        else bindText(insert, 8, finalTx.asset.network);
        if (finalTx.asset.contractAddress.empty()) sqlite3_bind_null(insert, 9);
        else bindText(insert, 9, finalTx.asset.contractAddress);
        bindText(insert, 10, TransactionEngine::statusToString(finalTx.status));
        sqlite3_bind_int64(insert, 11, now);
        sqlite3_bind_int64(insert, 12, now);
        const bool inserted = sqlite3_step(insert) == SQLITE_DONE;
        sqlite3_finalize(insert);
        if (!inserted)
            return rollback();

        TransactionStatus previous = TransactionStatus::INVALID;
        for (const auto* row : history.rows)
        {
            const TransactionStatus current = row->transaction.status;
            if (current == previous)
                continue;
            sqlite3_stmt* event = nullptr;
            if (sqlite3_prepare_v2(database, insertEventSql, -1, &event, nullptr) != SQLITE_OK)
                return rollback();
            bindText(event, 1, finalTx.transactionId);
            if (previous == TransactionStatus::INVALID) sqlite3_bind_null(event, 2);
            else bindText(event, 2, TransactionEngine::statusToString(previous));
            bindText(event, 3, TransactionEngine::statusToString(current));
            sqlite3_bind_int64(event, 4, now);
            bindText(event, 5, "Imported from CSV logical record " + std::to_string(row->rowNumber));
            const bool eventInserted = sqlite3_step(event) == SQLITE_DONE;
            sqlite3_finalize(event);
            if (!eventInserted)
                return rollback();
            previous = current;
        }
        ++report.importedRowCount;
    }

    if (report.conflictCount != 0)
    {
        // Keep the source-row ledger and failed batch while rolling back every
        // imported transaction/event, so a partial import cannot look complete.
        if (!executeSql("ROLLBACK TO SAVEPOINT legacy_transaction_import;") ||
            !executeSql("RELEASE SAVEPOINT legacy_transaction_import;"))
            return rollback();
        report.importedRowCount = 0;
        if (!failBatch())
            return rollback();
        return report;
    }

    if (!executeSql("RELEASE SAVEPOINT legacy_transaction_import;"))
        return rollback();

    sqlite3_stmt* completeBatch = nullptr;
    if (sqlite3_prepare_v2(database,
            "UPDATE migration_batches SET imported_row_count=?, reconciled_row_count=?, conflict_count=0, status='COMPLETE', completed_at_unix=? WHERE batch_id=?",
            -1, &completeBatch, nullptr) != SQLITE_OK)
        return rollback();
    sqlite3_bind_int64(completeBatch, 1, static_cast<sqlite3_int64>(report.importedRowCount));
    sqlite3_bind_int64(completeBatch, 2, static_cast<sqlite3_int64>(report.reconciledRowCount));
    sqlite3_bind_int64(completeBatch, 3, now);
    bindText(completeBatch, 4, batchId);
    const bool completedOk = sqlite3_step(completeBatch) == SQLITE_DONE;
    sqlite3_finalize(completeBatch);
    if (!completedOk || !commitTransaction())
        return rollback();

    report.success = true;
    return report;
}

void TransactionRepository::close()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (database)
    {
        sqlite3_close_v2(database);
        database = nullptr;
    }
    initialized = false;
    runtimeReadFailure_ = false;
}

TransactionSaveResult TransactionRepository::getLastSaveResult() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return lastSaveResult;
}

std::string TransactionRepository::getLastError() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return lastError_;
}

bool TransactionRepository::beginTransaction()
{
    return executeSql("BEGIN TRANSACTION;");
}

bool TransactionRepository::commitTransaction()
{
    return executeSql("COMMIT;");
}

bool TransactionRepository::rollbackTransaction()
{
    return executeSql("ROLLBACK;");
}

bool TransactionRepository::pragmaForeignKeys(bool enable)
{
    return executeSql(enable ? "PRAGMA foreign_keys = ON;" : "PRAGMA foreign_keys = OFF;");
}


