#include "../src/assets/AssetRegistry.h"
#include "../src/money/AmountParser.h"

#include "../src/core/TransactionEngine.h"
#include "../src/core/TransactionLogger.h"
#include "../src/core/TransactionRepository.h"
#include "../src/core/PaymentOrchestrationService.h"

#include "../src/protocol/JsonHandler.h"
#include "../src/transports/TcpTransport.h"
#include "../src/transports/SerialTransport.h"
#include "../src/transports/ITransport.h"
#include "../src/terminal/TerminalModel.h"
#include "../src/terminal/TerminalManager.h"
#include "../src/terminal/SimulatedTerminalTransport.h"
#include "../src/terminal/SimulatedTerminalProtocol.h"
#include "../src/terminal/SimulatedTerminalAdapter.h"
#include "../src/security/AuthManager.h"
#include "../src/communication/CommunicationManager.h"

#include <iostream>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>
#include <memory>
#include <string>
#include <cstdlib>
#include <chrono>
#include <atomic>
#include <limits>
#include <sqlite3.h>

class CountingSimulatedTransport final : public paybridge::transport::ITransport
{
public:
    CountingSimulatedTransport(paybridge::terminal::SimulatedTerminalOutcome outcome,
        std::shared_ptr<std::atomic<int>> sends)
        : inner(outcome), sends(std::move(sends)) {}
    paybridge::transport::TransportResult open() override { return inner.open(); }
    void close() noexcept override { inner.close(); }
    bool isOpen() const noexcept override { return inner.isOpen(); }
    paybridge::transport::TransportResult send(const std::vector<std::uint8_t>& bytes) override
    { ++(*sends); return inner.send(bytes); }
    paybridge::transport::TransportBytes receive(std::size_t maximum) override
    { return inner.receive(maximum); }
    void setReceiveTimeoutMilliseconds(std::uint32_t value) noexcept override
    { inner.setReceiveTimeoutMilliseconds(value); }
    void setSendTimeoutMilliseconds(std::uint32_t value) noexcept override
    { inner.setSendTimeoutMilliseconds(value); }
    std::string description() const override { return inner.description(); }
private:
    paybridge::terminal::SimulatedTerminalTransport inner;
    std::shared_ptr<std::atomic<int>> sends;
};

int main()
{
    const auto suiteRoot = std::filesystem::temp_directory_path() /
        ("paybridge_engine_suite_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(suiteRoot);
    const std::string testDatabasePath = (suiteRoot / "engine.sqlite").string();

    // Repository tests use unique files under the OS temporary directory and
    // never touch the application's configured database or CSV history.
    {
        const auto base = std::filesystem::temp_directory_path() /
            ("paybridge_repository_" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        const std::string dbPath = base.string() + ".db";
        const std::string futurePath = base.string() + "_future.db";
        auto removeDb = [](const std::string& path) {
            std::error_code ec;
            std::filesystem::remove(path, ec);
            std::filesystem::remove(path + "-wal", ec);
            std::filesystem::remove(path + "-shm", ec);
        };
        paybridge::assets::AssetRegistry repositoryRegistry;
        const auto usdAsset = repositoryRegistry.findFiat("USD");
        assert(usdAsset);
        Transaction tx;
        tx.transactionId = "PB-REPOSITORY-TEST-1";
        tx.externalReference = "REPOSITORY-TEST-REF-1";
        tx.amountMinor = 1234;
        tx.currency = "USD";
        tx.status = TransactionStatus::CREATED;
        tx.asset = *usdAsset;
        tx.money = {1234, usdAsset->decimalPrecision};

        {
            TransactionRepository repo(dbPath);
            assert(repo.initialize());
            assert(repo.isInitialized());
            assert(repo.saveTransaction(tx) == TransactionSaveResult::NEW_TRANSACTION);
            assert(repo.saveTransaction(tx) == TransactionSaveResult::EXISTING_IDENTICAL);
            Transaction samePayment = tx;
            samePayment.transactionId = "PB-REPOSITORY-TEST-RETRY-ID";
            assert(repo.saveTransaction(samePayment) == TransactionSaveResult::EXISTING_IDENTICAL);
            Transaction conflict = tx;
            conflict.money.units++;
            assert(repo.saveTransaction(conflict) == TransactionSaveResult::CONFLICT);

            sqlite3* raw = nullptr;
            assert(sqlite3_open(dbPath.c_str(), &raw) == SQLITE_OK);
            sqlite3_stmt* query = nullptr;
            assert(sqlite3_prepare_v2(raw, "SELECT count(*) FROM transaction_lifecycle_events", -1, &query, nullptr) == SQLITE_OK);
            assert(sqlite3_step(query) == SQLITE_ROW && sqlite3_column_int(query, 0) == 1);
            sqlite3_finalize(query);
            assert(sqlite3_exec(raw,
                "CREATE TRIGGER reject_event BEFORE INSERT ON transaction_lifecycle_events BEGIN SELECT RAISE(ABORT,'injected'); END;",
                nullptr, nullptr, nullptr) == SQLITE_OK);
            sqlite3_close(raw);

            Transaction rejected = tx;
            rejected.transactionId = "PB-REPOSITORY-TEST-ATOMIC";
            rejected.externalReference = "REPOSITORY-TEST-ATOMIC";
            assert(repo.saveTransaction(rejected) == TransactionSaveResult::DATABASE_ERROR);
            assert(repo.loadTransactions().size() == 1); // Insert rolled back with its event.

            assert(sqlite3_open(dbPath.c_str(), &raw) == SQLITE_OK);
            assert(sqlite3_exec(raw, "DROP TRIGGER reject_event", nullptr, nullptr, nullptr) == SQLITE_OK);
            assert(sqlite3_close(raw) == SQLITE_OK);

            assert(sqlite3_open(dbPath.c_str(), &raw) == SQLITE_OK);
            assert(sqlite3_exec(raw,
                "CREATE TRIGGER reject_sent_event BEFORE INSERT ON transaction_lifecycle_events "
                "WHEN NEW.to_status='SENT' BEGIN SELECT RAISE(ABORT,'injected'); END;",
                nullptr, nullptr, nullptr) == SQLITE_OK);
            sqlite3_close(raw);
            Transaction sent = tx;
            sent.status = TransactionStatus::SENT;
            assert(!repo.updateTransactionStatus(sent));
            auto stillCreated = repo.loadTransactions();
            assert(stillCreated.size() == 1 && stillCreated[0].status == TransactionStatus::CREATED);
            assert(sqlite3_open(dbPath.c_str(), &raw) == SQLITE_OK);
            assert(sqlite3_exec(raw, "DROP TRIGGER reject_sent_event", nullptr, nullptr, nullptr) == SQLITE_OK);
            sqlite3_close(raw);
            assert(repo.updateTransactionStatus(sent));
            Transaction unknown = tx;
            unknown.status = TransactionStatus::UNKNOWN;
            assert(repo.updateTransactionStatus(unknown));
            Transaction approved = tx;
            approved.status = TransactionStatus::APPROVED;
            assert(repo.updateTransactionStatus(approved));
            assert(!repo.updateTransactionStatus(sent)); // Terminal state cannot move backwards.

            auto loaded = repo.loadTransactions();
            assert(loaded.size() == 1 && loaded[0].status == TransactionStatus::APPROVED);
            repo.close();
            assert(!repo.isInitialized());
            assert(repo.initialize());
            loaded = repo.loadTransactions();
            assert(loaded.size() == 1 && loaded[0].status == TransactionStatus::APPROVED);

            assert(sqlite3_open(dbPath.c_str(), &raw) == SQLITE_OK);
            query = nullptr;
            assert(sqlite3_prepare_v2(raw,
                "SELECT from_status,to_status FROM transaction_lifecycle_events ORDER BY event_id", -1, &query, nullptr) == SQLITE_OK);
            const std::vector<std::pair<std::string, std::string>> expected = {
                {"", "CREATED"}, {"CREATED", "SENT"}, {"SENT", "UNKNOWN"}, {"UNKNOWN", "APPROVED"}};
            for (const auto& pair : expected)
            {
                assert(sqlite3_step(query) == SQLITE_ROW);
                const auto from = sqlite3_column_text(query, 0);
                const auto to = sqlite3_column_text(query, 1);
                assert((from ? reinterpret_cast<const char*>(from) : "") == pair.first);
                assert((to ? reinterpret_cast<const char*>(to) : "") == pair.second);
            }
            assert(sqlite3_step(query) == SQLITE_DONE);
            sqlite3_finalize(query);
            assert(sqlite3_close(raw) == SQLITE_OK);
            repo.close();
        }

        // Independently verify UNKNOWN round-trips without becoming INVALID.
        tx.status = TransactionStatus::UNKNOWN;
        tx.transactionId = "PB-REPOSITORY-UNKNOWN";
        tx.externalReference = "REPOSITORY-UNKNOWN";
        {
            TransactionRepository repo(dbPath);
            assert(repo.initialize());
            assert(repo.saveTransaction(tx) == TransactionSaveResult::NEW_TRANSACTION);
            repo.close();
            assert(repo.initialize());
            const auto loaded = repo.loadTransactions();
            assert(loaded.size() == 2);
            bool foundUnknown = false;
            for (const auto& item : loaded)
                if (item.transactionId == tx.transactionId)
                    foundUnknown = item.status == TransactionStatus::UNKNOWN;
            assert(foundUnknown);
        }

        sqlite3* future = nullptr;
        assert(sqlite3_open(futurePath.c_str(), &future) == SQLITE_OK);
        assert(sqlite3_exec(future,
            "CREATE TABLE schema_metadata(metadata_key TEXT PRIMARY KEY, metadata_value TEXT);"
            "INSERT INTO schema_metadata VALUES('schema_version','999');",
            nullptr, nullptr, nullptr) == SQLITE_OK);
        sqlite3_close(future);
        {
            TransactionRepository repo(futurePath);
            assert(!repo.initialize());
            assert(!repo.isInitialized());
            assert(!repo.getLastError().empty());
        }
        assert(sqlite3_open(futurePath.c_str(), &future) == SQLITE_OK);
        sqlite3_stmt* futureVersion = nullptr;
        assert(sqlite3_prepare_v2(future,
            "SELECT metadata_value FROM schema_metadata WHERE metadata_key='schema_version'",
            -1, &futureVersion, nullptr) == SQLITE_OK);
        assert(sqlite3_step(futureVersion) == SQLITE_ROW);
        assert(std::string(reinterpret_cast<const char*>(sqlite3_column_text(futureVersion, 0))) == "999");
        sqlite3_finalize(futureVersion);
        sqlite3_close(future);

        const std::string corruptPath = base.string() + "_corrupt.db";
        {
            TransactionRepository repo(corruptPath);
            assert(repo.initialize());
            assert(repo.saveTransaction(tx) == TransactionSaveResult::NEW_TRANSACTION);
            repo.close();
        }
        {
            TransactionRepository repo(corruptPath);
            assert(repo.initialize());
            sqlite3* corrupt = nullptr;
            assert(sqlite3_open(corruptPath.c_str(), &corrupt) == SQLITE_OK);
            assert(sqlite3_exec(corrupt,
                "PRAGMA ignore_check_constraints=ON; UPDATE transactions SET lifecycle_state='ALIEN';",
                nullptr, nullptr, nullptr) == SQLITE_OK);
            sqlite3_close(corrupt);
            assert(repo.loadTransactions().empty());
            assert(repo.getLastError().find("lifecycle state") != std::string::npos);
        }

        // Two independent connections racing the same external reference must
        // serialize and report one insert plus one identical replay.
        const std::string concurrentPath = base.string() + "_concurrent.db";
        Transaction concurrentTx = tx;
        concurrentTx.transactionId = "PB-REPOSITORY-CONCURRENT";
        concurrentTx.externalReference = "REPOSITORY-CONCURRENT";
        concurrentTx.status = TransactionStatus::CREATED;
        TransactionSaveResult concurrentResults[2] = {
            TransactionSaveResult::DATABASE_ERROR, TransactionSaveResult::DATABASE_ERROR};
        std::thread writers[2] = {
            std::thread([&] { TransactionRepository r(concurrentPath); assert(r.initialize()); concurrentResults[0] = r.saveTransaction(concurrentTx); }),
            std::thread([&] { TransactionRepository r(concurrentPath); assert(r.initialize()); concurrentResults[1] = r.saveTransaction(concurrentTx); })};
        writers[0].join();
        writers[1].join();
        assert((concurrentResults[0] == TransactionSaveResult::NEW_TRANSACTION && concurrentResults[1] == TransactionSaveResult::EXISTING_IDENTICAL) ||
               (concurrentResults[1] == TransactionSaveResult::NEW_TRANSACTION && concurrentResults[0] == TransactionSaveResult::EXISTING_IDENTICAL));

        const std::string blocker = base.string() + "_blocker";
        { std::ofstream file(blocker); file << "block"; }
        TransactionRepository retry(blocker + "\\db.sqlite");
        assert(!retry.initialize());
        std::filesystem::remove(blocker);
        assert(retry.initialize()); // Failed initialization left the object retryable.
        retry.close();

        const std::string pageDbPath = base.string() + "_pages.db";
        {
            TransactionRepository pageRepo(pageDbPath);
            assert(pageRepo.initialize());
            for (const std::string& id : {"PB-PAGE-A", "PB-PAGE-B", "PB-PAGE-C", "PB-PAGE-D", "PB-PAGE-E"})
            {
                Transaction row = tx;
                row.transactionId = id;
                row.externalReference = std::string("REF-") + id;
                row.money = {123456789, 2};
                row.amountMinor = row.money.units;
                row.asset = *usdAsset;
                row.currency = "USD";
                row.status = TransactionStatus::CREATED;
                assert(pageRepo.saveTransaction(row) == TransactionSaveResult::NEW_TRANSACTION);
            }
            sqlite3* pageDb = nullptr;
            assert(sqlite3_open(pageDbPath.c_str(), &pageDb) == SQLITE_OK);
            assert(sqlite3_exec(pageDb,
                "UPDATE transactions SET created_at_unix=100,updated_at_unix=100;"
                "UPDATE transactions SET lifecycle_state='APPROVED' WHERE transaction_id IN ('PB-PAGE-A','PB-PAGE-D');"
                "UPDATE transactions SET lifecycle_state='DECLINED' WHERE transaction_id='PB-PAGE-B';"
                "UPDATE transactions SET lifecycle_state='FAILED' WHERE transaction_id='PB-PAGE-E';"
                "INSERT INTO replay_records(request_id,transaction_id,external_reference,canonical_request_hash,payment_fingerprint,request_timestamp_unix,request_expiry_unix,replay_state,created_at_unix,updated_at_unix,terminal_id)"
                " VALUES('PAGE-REQUEST-A','PB-PAGE-A','REF-PB-PAGE-A','hash-hidden','fingerprint-hidden',100,200,'COMPLETED',100,101,'SIM-READ-001');",
                nullptr, nullptr, nullptr) == SQLITE_OK);
            sqlite3_close(pageDb);

            const auto first = pageRepo.listTransactions(2);
            assert(first.success && first.validRequest && first.transactions.size() == 2);
            assert(first.transactions[0].transactionId == "PB-PAGE-E");
            assert(first.transactions[1].transactionId == "PB-PAGE-D");
            assert(first.hasMore && first.nextCursor);
            assert(first.nextCursor->createdAtUnix == 100);
            assert(first.nextCursor->transactionId == "PB-PAGE-D");

            const auto second = pageRepo.listTransactions(2, std::nullopt, first.nextCursor);
            assert(second.success && second.transactions.size() == 2 && second.hasMore);
            assert(second.transactions[0].transactionId == "PB-PAGE-C");
            assert(second.transactions[1].transactionId == "PB-PAGE-B");
            const auto third = pageRepo.listTransactions(2, std::nullopt, second.nextCursor);
            assert(third.success && third.transactions.size() == 1 && !third.hasMore);
            assert(third.transactions[0].transactionId == "PB-PAGE-A");
            assert(!third.nextCursor);

            const auto approved = pageRepo.listTransactions(10, TransactionStatus::APPROVED);
            assert(approved.success && approved.transactions.size() == 2);
            assert(approved.transactions[0].transactionId == "PB-PAGE-D");
            assert(approved.transactions[1].transactionId == "PB-PAGE-A");
            const auto empty = pageRepo.listTransactions(10, TransactionStatus::UNKNOWN);
            assert(empty.success && empty.transactions.empty() && !empty.hasMore);
            assert(!pageRepo.listTransactions(0).validRequest);
            assert(!pageRepo.listTransactions(10, std::nullopt, TransactionCursor{-1, "PB-PAGE-A"}).validRequest);
            assert(!pageRepo.listTransactions(10, std::nullopt, TransactionCursor{100, ""}).validRequest);
            assert(!pageRepo.listTransactions(10, std::nullopt,
                TransactionCursor{100, std::string(257, 'x')}).validRequest);
            const auto capped = pageRepo.listTransactions(1000);
            assert(capped.success && capped.transactions.size() == 5 && !capped.hasMore);

            const auto detail = pageRepo.findTransactionById("PB-PAGE-A");
            assert(detail);
            assert(detail->transactionId == "PB-PAGE-A");
            assert(detail->money.units == 123456789 && detail->money.precision == 2);
            assert(detail->asset.type == paybridge::assets::AssetType::FIAT);
            assert(detail->asset.code == "USD" && detail->asset.decimalPrecision == 2);
            assert(detail->status == TransactionStatus::APPROVED);
            assert(detail->createdAtUnix == 100 && detail->updatedAtUnix == 100);
            assert(detail->terminalId == "SIM-READ-001");
            assert(!pageRepo.findTransactionById("missing"));
            assert(!pageRepo.findTransactionById(std::string(257, 'x')));
        }
        removeDb(pageDbPath);
        removeDb(dbPath);
        removeDb(futurePath);
        removeDb(concurrentPath);
        removeDb(corruptPath);
        removeDb(blocker + "\\db.sqlite");
        std::error_code cleanupError;
        std::filesystem::remove(base.string() + "_blocker", cleanupError);
        std::cout << "SQLite repository tests passed!" << std::endl;
    }

    // Engine restore tests use SQLite directly and register configured assets
    // before initialization, including non-default digital assets.
    {
        const std::string digitalDb = (suiteRoot / "digital-recovery.sqlite").string();
        std::string originalId;
        {
            TransactionEngine engine(digitalDb, std::string{});
            assert(engine.assets().registerUsdt("TRON_TESTNET", "CONFIGURED_TEST_CONTRACT", 6));
            assert(engine.initialize());
            const auto usdt = engine.assets().resolveDigitalAsset(
                "USDT", "TRON_TESTNET", "CONFIGURED_TEST_CONTRACT");
            assert(usdt);
            Transaction created = engine.createTransaction(
                paybridge::money::Money{1234567, 6}, *usdt, "USDT-RECOVERY-001");
            assert(created.status == TransactionStatus::CREATED);
            originalId = created.transactionId;
            assert(engine.updateStatus(created, TransactionStatus::UNKNOWN));
        }
        {
            TransactionEngine restored(digitalDb, std::string{});
            assert(restored.assets().registerUsdt("TRON_TESTNET", "CONFIGURED_TEST_CONTRACT", 6));
            assert(restored.initialize());
            const auto usdt = restored.assets().resolveDigitalAsset(
                "USDT", "TRON_TESTNET", "CONFIGURED_TEST_CONTRACT");
            assert(usdt);
            Transaction recovered = restored.createTransaction(
                paybridge::money::Money{1234567, 6}, *usdt, "USDT-RECOVERY-001");
            assert(recovered.transactionId == originalId);
            assert(recovered.status == TransactionStatus::UNKNOWN);
        }
    }

    // Separate engine instances see the canonical persisted state and reject
    // a stale lifecycle update rather than overwriting a concurrent change.
    {
        TransactionEngine first(testDatabasePath, std::string{});
        TransactionEngine second(testDatabasePath, std::string{});
        Transaction a = first.createTransaction(3210, "USD", "CROSS-ENGINE-001");
        Transaction b = second.createTransaction(3210, "USD", "CROSS-ENGINE-001");
        assert(a.transactionId == b.transactionId);
        assert(first.updateStatus(a, TransactionStatus::SENT));
        assert(!second.updateStatus(b, TransactionStatus::FAILED));
        Transaction refreshed = second.createTransaction(3210, "USD", "CROSS-ENGINE-001");
        assert(refreshed.status == TransactionStatus::SENT);
        assert(refreshed.transactionId == a.transactionId);
    }

    // Import legacy CSV snapshots once, preserve UNKNOWN, and block changed
    // source bytes after a completed cutover rather than replaying the file.
    {
        const std::string migrationCsv = (suiteRoot / "migration-history.csv").string();
        const std::string migrationDb = (suiteRoot / "migration-history.sqlite").string();
        TransactionLogger::setFilePath(migrationCsv);
        Transaction migrationTx;
        migrationTx.transactionId = "PB-MIGRATION-HISTORY-001";
        migrationTx.externalReference = "MIGRATION-HISTORY-001";
        migrationTx.amountMinor = 2500;
        migrationTx.currency = "USD";
        migrationTx.status = TransactionStatus::CREATED;
        paybridge::assets::AssetRegistry migrationAssets;
        const auto usd = migrationAssets.findFiat("USD");
        assert(usd);
        migrationTx.asset = *usd;
        migrationTx.money = {2500, usd->decimalPrecision};
        assert(TransactionLogger::logTransaction(migrationTx));
        migrationTx.status = TransactionStatus::SENT;
        assert(TransactionLogger::logTransaction(migrationTx));
        migrationTx.status = TransactionStatus::UNKNOWN;
        assert(TransactionLogger::logTransaction(migrationTx));

        std::string migratedId;
        {
            TransactionEngine migrated(migrationDb, migrationCsv);
            assert(migrated.initialize());
            Transaction recovered = migrated.createTransaction(
                2500, "USD", "MIGRATION-HISTORY-001");
            assert(recovered.status == TransactionStatus::UNKNOWN);
            migratedId = recovered.transactionId;
        }
        {
            TransactionEngine repeated(migrationDb, migrationCsv);
            assert(repeated.initialize());
            Transaction recovered = repeated.createTransaction(
                2500, "USD", "MIGRATION-HISTORY-001");
            assert(recovered.transactionId == migratedId);
            assert(recovered.status == TransactionStatus::UNKNOWN);
            assert(repeated.updateStatus(recovered, TransactionStatus::APPROVED));
        }
        {
            TransactionEngine afterCutoverWrite(migrationDb, migrationCsv);
            assert(afterCutoverWrite.initialize());
            Transaction recovered = afterCutoverWrite.createTransaction(
                2500, "USD", "MIGRATION-HISTORY-001");
            assert(recovered.transactionId == migratedId);
            assert(recovered.status == TransactionStatus::APPROVED);
        }

        migrationTx.status = TransactionStatus::APPROVED;
        assert(TransactionLogger::logTransaction(migrationTx));
        {
            TransactionEngine changedSource(migrationDb, migrationCsv);
            assert(!changedSource.initialize());
            assert(!changedSource.persistenceError().empty());
        }
        {
            TransactionRepository verify(migrationDb);
            assert(verify.initialize());
            const auto rows = verify.loadTransactions();
            assert(rows.size() == 1 && rows[0].status == TransactionStatus::APPROVED);
        }
    }

    // Mixed historical six-column and verified twelve-column rows:
    // retain ambiguous rows for audit while importing verified lifecycle rows.
    {
        const std::string mixedCsv = (suiteRoot / "migration-mixed-history.csv").string();
        const std::string mixedDb = (suiteRoot / "migration-mixed-history.sqlite").string();

        {
            std::ofstream file(mixedCsv, std::ios::binary);
            file << "Transaction ID,Amount,Currency,Status,Timestamp,Environment\r\n"
                 << "PB-000001,123.00,AED,RECEIVED,2026-11-24 01:06:59,SIMULATION\r\n"
                 << "PB-000001,123.00,AED,RECEIVED,2026-11-24 01:06:59,SIMULATION\r\n"
                 << "PB-000001,12.00,AED,RECEIVED,2026-11-24 01:09:25,SIMULATION\r\n"
                 << "PB-000001,54.00,AED,RECEIVED,2026-11-24 01:09:37,SIMULATION\r\n"
                 << "PB-000001,57.00,AED,RECEIVED,2026-11-24 01:09:50,SIMULATION\r\n"
                 << "PB-OLD-006,12.00,AED,RECEIVED,2026-11-24 01:11:53,SIMULATION\r\n"
                 << "PB-OLD-007,45.00,AED,RECEIVED,2026-11-24 01:12:01,SIMULATION\r\n"
                 << "PB-OLD-008,90.00,AED,RECEIVED,2026-11-24 01:12:05,SIMULATION\r\n"
                 << "PB-NEW-001,POS-AUTH-001,1000,AED,CREATED,2026-11-24 16:54:20,SIMULATION,FIAT,AED,,,2\r\n"
                 << "PB-NEW-001,POS-AUTH-001,1000,AED,SENT,2026-11-24 16:54:20,SIMULATION,FIAT,AED,,,2\r\n"
                 << "PB-NEW-001,POS-AUTH-001,1000,AED,RECEIVED,2026-11-24 16:54:20,SIMULATION,FIAT,AED,,,2\r\n";
        }

        TransactionEngine mixed(mixedDb, mixedCsv);
        assert(mixed.initialize());

        Transaction recovered = mixed.createTransaction(
            1000, "AED", "POS-AUTH-001");
        assert(recovered.status == TransactionStatus::RECEIVED);

        sqlite3* raw = nullptr;
        assert(sqlite3_open(mixedDb.c_str(), &raw) == SQLITE_OK);

        sqlite3_stmt* auditQuery = nullptr;
        assert(sqlite3_prepare_v2(raw,
            "SELECT COUNT(*), "
            "SUM(CASE WHEN transaction_id IS NULL THEN 1 ELSE 0 END) "
            "FROM migration_legacy_rows",
            -1, &auditQuery, nullptr) == SQLITE_OK);
        assert(sqlite3_step(auditQuery) == SQLITE_ROW);
        assert(sqlite3_column_int(auditQuery, 0) == 11);
        assert(sqlite3_column_int(auditQuery, 1) == 8);
        sqlite3_finalize(auditQuery);
        assert(sqlite3_close(raw) == SQLITE_OK);
    }
    // Seven-column legacy fiat rows infer exact precision from the registry.
    {
        const std::string legacyCsv = (suiteRoot / "migration-legacy-seven.csv").string();
        const std::string legacyDb = (suiteRoot / "migration-legacy-seven.sqlite").string();
        {
            std::ofstream file(legacyCsv, std::ios::binary);
            file << "TransactionID,ExternalReference,AmountMinor,Currency,Status,Timestamp,Environment\r\n"
                 << "PB-LEGACY-7,LEGACY-7,1234,USD,CREATED,2026-01-01 10:00:00,SIMULATION\r\n"
                 << "PB-LEGACY-7,LEGACY-7,1234,USD,UNKNOWN,2026-01-01 10:01:00,SIMULATION\r\n";
        }
        TransactionEngine legacy(legacyDb, legacyCsv);
        assert(legacy.initialize());
        Transaction recovered = legacy.createTransaction(1234, "USD", "LEGACY-7");
        assert(recovered.status == TransactionStatus::UNKNOWN);
        assert(recovered.money.precision == 2);
    }

    // Simulator/client-local attempts and server-authoritative records use
    // distinct SQLite files and cannot observe or overwrite each other.
    {
        const std::string serverDb = (suiteRoot / "server.sqlite").string();
        const std::string simulatorDb = (suiteRoot / "simulator.sqlite").string();
        assert(std::filesystem::absolute(serverDb).lexically_normal() !=
               std::filesystem::absolute(simulatorDb).lexically_normal());
        TransactionEngine server(serverDb, std::string{});
        TransactionEngine simulator(simulatorDb, std::string{});
        Transaction serverTx = server.createTransaction(500, "USD", "ISOLATED-REF");
        Transaction simulatorTx = simulator.createTransaction(500, "USD", "ISOLATED-REF");
        assert(serverTx.status == TransactionStatus::CREATED);
        assert(simulatorTx.status == TransactionStatus::CREATED);
        assert(serverTx.transactionId != simulatorTx.transactionId);
        assert(server.updateStatus(serverTx, TransactionStatus::SENT));
        Transaction clientState = simulator.createTransaction(500, "USD", "ISOLATED-REF");
        assert(clientState.status == TransactionStatus::CREATED);
    }

    // Malformed CSV and conflicting transaction histories are retained as
    // failed migration batches and block engine startup without partial rows.
    {
        const std::string malformedCsv = (suiteRoot / "migration-malformed.csv").string();
        const std::string malformedDb = (suiteRoot / "migration-malformed.sqlite").string();
        {
            std::ofstream file(malformedCsv, std::ios::binary);
            file << "TransactionID,ExternalReference,AmountMinor,Currency,Status,Timestamp,Environment,AssetType,AssetCode,AssetNetwork,ContractAddress,DecimalPrecision\n"
                 << "PB-MALFORMED,REF-MALFORMED,1000,USD,CREATED,2026-01-01,SIMULATION,FIAT,USD,,,2\n"
                 << "\"unterminated,CSV,row\n";
        }
        TransactionEngine malformed(malformedDb, malformedCsv);
        assert(!malformed.initialize());
        assert(malformed.persistenceError().find("Malformed") != std::string::npos);
        TransactionRepository failedBatch(malformedDb);
        assert(failedBatch.initialize());
        assert(failedBatch.loadTransactions().empty());
        sqlite3* raw = nullptr;
        assert(sqlite3_open(malformedDb.c_str(), &raw) == SQLITE_OK);
        sqlite3_stmt* batchQuery = nullptr;
        assert(sqlite3_prepare_v2(raw,
            "SELECT status, conflict_count FROM migration_batches",
            -1, &batchQuery, nullptr) == SQLITE_OK);
        assert(sqlite3_step(batchQuery) == SQLITE_ROW);
        assert(std::string(reinterpret_cast<const char*>(sqlite3_column_text(batchQuery, 0))) == "FAILED");
        assert(sqlite3_column_int(batchQuery, 1) > 0);
        sqlite3_finalize(batchQuery);
        assert(sqlite3_close(raw) == SQLITE_OK);
    }
    {
        const std::string conflictCsv = (suiteRoot / "migration-conflict.csv").string();
        const std::string conflictDb = (suiteRoot / "migration-conflict.sqlite").string();
        std::ofstream file(conflictCsv, std::ios::binary);
        file << "TransactionID,ExternalReference,AmountMinor,Currency,Status,Timestamp,Environment,AssetType,AssetCode,AssetNetwork,ContractAddress,DecimalPrecision\n"
             << "PB-CONFLICT-A,REF-CONFLICT,1000,USD,CREATED,2026-01-01,SIMULATION,FIAT,USD,,,2\n"
             << "PB-CONFLICT-B,REF-CONFLICT,1000,USD,CREATED,2026-01-01,SIMULATION,FIAT,USD,,,2\n";
        file.close();
        TransactionEngine conflict(conflictDb, conflictCsv);
        assert(!conflict.initialize());
        assert(conflict.persistenceError().find("conflicting") != std::string::npos);
        TransactionRepository failedConflict(conflictDb);
        assert(failedConflict.initialize());
        assert(failedConflict.loadTransactions().empty());
    }

    TransactionLogger::resetFilePath();

    const std::string testToken = AuthManager::generateBearerToken();
    assert(!testToken.empty());
    assert(AuthManager::validateBearerTokenFormat(testToken));

    AuthManager validAuth = AuthManager::fromBearerToken(testToken, AuthManager::Mode::DEVELOPMENT);
    assert(validAuth.isConfigured());
    assert(validAuth.verifyAuthorization("Authorization: Bearer " + testToken) ==
           AuthManager::VerificationResult::ALLOWED);
    assert(validAuth.verifyAuthorization("") == AuthManager::VerificationResult::MISSING);
    assert(validAuth.verifyAuthorization("Bearer " + testToken) ==
           AuthManager::VerificationResult::MALFORMED);
    assert(validAuth.verifyAuthorization("Authorization: Basic abc") ==
           AuthManager::VerificationResult::MALFORMED);

    const std::string invalidToken = AuthManager::generateBearerToken();
    assert(!invalidToken.empty());
    assert(validAuth.verifyAuthorization("Authorization: Bearer " + invalidToken) ==
           AuthManager::VerificationResult::INVALID);

    AuthManager unconfigured = AuthManager::fromBearerToken("", AuthManager::Mode::PRODUCTION);
    assert(!unconfigured.isConfigured());
    assert(unconfigured.verifyAuthorization("") ==
           AuthManager::VerificationResult::NOT_CONFIGURED);

    paybridge::assets::AssetRegistry registry;

    const auto usd = registry.findFiat("USD");
    assert(usd && usd->decimalPrecision == 2);

    const auto jpy = registry.findFiat("JPY");
    assert(jpy && jpy->decimalPrecision == 0);

    const auto kwd = registry.findFiat("KWD");
    assert(kwd && kwd->decimalPrecision == 3);

    const auto parsedUsd = paybridge::money::parseAmount("125.50", 2);
    assert(parsedUsd.success());
    assert(parsedUsd.money.units == 12550);
    assert(paybridge::money::formatAmount(parsedUsd.money) == "125.50");

    assert(!paybridge::money::parseAmount("125.501", 2).success());
    assert(!paybridge::money::parseAmount("-1.00", 2).success());
    assert(!paybridge::money::parseAmount("0.00", 2).success());
    assert(!paybridge::money::parseAmount("abc", 2).success());

    assert(registry.registerUsdt("TRON_TESTNET", "CONFIGURED_TEST_CONTRACT", 6));
    const auto usdt = registry.resolveDigitalAsset(
        "USDT",
        "TRON_TESTNET",
        "CONFIGURED_TEST_CONTRACT"
    );
    assert(usdt && usdt->decimalPrecision == 6);

    assert(!registry.resolveDigitalAsset(
        "USDT",
        "UNCONFIGURED_NETWORK",
        "UNCONFIGURED_CONTRACT"
    ));

    assert(registry.registerUsdt("OTHER_TESTNET", "ANOTHER_CONFIGURED_CONTRACT", 6));
    const auto assetReadModels = registry.readModels();
    assert(!assetReadModels.empty());
    const auto repeatedAssetReadModels = registry.readModels();
    assert(assetReadModels.size() == repeatedAssetReadModels.size());
    for (std::size_t index = 0; index < assetReadModels.size(); ++index)
    {
        assert(!assetReadModels[index].type.empty());
        assert(assetReadModels[index].type == repeatedAssetReadModels[index].type);
        assert(assetReadModels[index].code == repeatedAssetReadModels[index].code);
        assert(assetReadModels[index].precision == repeatedAssetReadModels[index].precision);
        assert(assetReadModels[index].enabled);
        if (index > 0)
        {
            const auto& previous = assetReadModels[index - 1];
            const auto& current = assetReadModels[index];
            assert(previous.type < current.type ||
                (previous.type == current.type && previous.code < current.code) ||
                (previous.type == current.type && previous.code == current.code &&
                 previous.precision <= current.precision));
        }
    }
    std::size_t usdtReadModelCount = 0;
    for (const auto& asset : assetReadModels)
        if (asset.code == "USDT" && asset.type == "DIGITAL_ASSET" && asset.precision == 6)
            ++usdtReadModelCount;
    assert(usdtReadModelCount == 1);

    const std::string testLog = (suiteRoot / "transaction_engine_tests.csv").string();
    const std::string blockerPath = (suiteRoot / "database_blocker").string();

    // Use a separate log so production transaction history is untouched.
    TransactionLogger::setFilePath(testLog);
    std::filesystem::remove(testLog);
    std::filesystem::remove_all(blockerPath);

    {
        TransactionEngine engine(testDatabasePath, std::string{});

        // Test 1: Successful payment
        Transaction approved =
            engine.createTransaction(10000, "AED", "POS-1001");

        assert(approved.status == TransactionStatus::CREATED);
        assert(!approved.transactionId.empty());

        assert(engine.updateStatus(approved, TransactionStatus::SENT));
        assert(engine.updateStatus(approved, TransactionStatus::RECEIVED));
        assert(engine.updateStatus(approved, TransactionStatus::APPROVED));

        // Test 2: Approved transaction is terminal
        assert(!engine.updateStatus(approved, TransactionStatus::CREATED));
        assert(!engine.updateStatus(approved, TransactionStatus::DECLINED));

        // Test 3: Declined payment
        Transaction declined =
            engine.createTransaction(5000, "AED", "POS-1002");

        assert(engine.updateStatus(declined, TransactionStatus::SENT));
        assert(engine.updateStatus(declined, TransactionStatus::RECEIVED));
        assert(engine.updateStatus(declined, TransactionStatus::DECLINED));

        assert(!engine.updateStatus(declined, TransactionStatus::APPROVED));

        // Test 4: Failed payment
        Transaction failed =
            engine.createTransaction(2000, "AED", "POS-1003");

        assert(engine.updateStatus(failed, TransactionStatus::FAILED));
        assert(!engine.updateStatus(failed, TransactionStatus::SENT));

        // Test 5: Invalid amount
        Transaction invalidAmount =
            engine.createTransaction(0, "AED", "POS-1004");

        assert(invalidAmount.status == TransactionStatus::INVALID);

        // Test 6: Invalid currency
        Transaction invalidCurrency =
            engine.createTransaction(10000, "abc", "POS-1005");

        assert(invalidCurrency.status == TransactionStatus::INVALID);

        // Test 7: Empty transaction ID
        Transaction emptyTransaction;
        assert(!engine.updateStatus(emptyTransaction, TransactionStatus::SENT));

        // Test 8: Invalid transaction cannot progress
        assert(!engine.updateStatus(invalidAmount, TransactionStatus::SENT));

        // Test 9: Duplicate request returns original transaction
        Transaction original =
            engine.createTransaction(7500, "AED", "POS-2001");

        Transaction duplicate =
            engine.createTransaction(7500, "AED", "POS-2001");

        assert(original.transactionId == duplicate.transactionId);
        assert(duplicate.status == original.status);

        // Test 10: Same reference with different amount is rejected
        Transaction amountMismatch =
            engine.createTransaction(9000, "AED", "POS-2001");

        assert(amountMismatch.status == TransactionStatus::INVALID);

        // Test 11: Same reference with different currency is rejected
        Transaction currencyMismatch =
            engine.createTransaction(7500, "USD", "POS-2001");

        assert(currencyMismatch.status == TransactionStatus::INVALID);

        // Test 12: Duplicate retry returns latest stored status
        assert(engine.updateStatus(original, TransactionStatus::SENT));

        Transaction retry =
            engine.createTransaction(7500, "AED", "POS-2001");

        assert(retry.transactionId == original.transactionId);
        assert(retry.status == TransactionStatus::SENT);

        // Test 13: Empty external reference is rejected
        Transaction emptyReference =
            engine.createTransaction(1000, "AED", "");

        assert(emptyReference.status == TransactionStatus::INVALID);

        // Test 18: Modified transaction ID is rejected
        Transaction idCheck =
            engine.createTransaction(1000, "AED", "POS-SECURITY-001");

        Transaction alteredId = idCheck;
        alteredId.transactionId = "PB-FAKE-ID";

        assert(!engine.updateStatus(alteredId, TransactionStatus::SENT));
        assert(idCheck.status == TransactionStatus::CREATED);

        assert(engine.updateStatus(idCheck, TransactionStatus::SENT));

        // Test 19: Modified amount is rejected
        Transaction amountCheck =
            engine.createTransaction(2000, "AED", "POS-SECURITY-002");

        Transaction alteredAmount = amountCheck;
        alteredAmount.amountMinor = 999999;

        assert(!engine.updateStatus(alteredAmount, TransactionStatus::SENT));
        assert(amountCheck.status == TransactionStatus::CREATED);

        assert(engine.updateStatus(amountCheck, TransactionStatus::SENT));

        // Test 20: Modified currency is rejected
        Transaction currencyCheck =
            engine.createTransaction(3000, "AED", "POS-SECURITY-003");

        Transaction alteredCurrency = currencyCheck;
        alteredCurrency.currency = "USD";

        assert(!engine.updateStatus(alteredCurrency, TransactionStatus::SENT));
        assert(currencyCheck.status == TransactionStatus::CREATED);

        assert(engine.updateStatus(currencyCheck, TransactionStatus::SENT));

        // Test 21: Stale transaction status is rejected
        Transaction staleSource =
            engine.createTransaction(4000, "AED", "POS-SECURITY-004");

        Transaction staleCopy = staleSource;

        assert(engine.updateStatus(staleSource, TransactionStatus::SENT));
        assert(engine.updateStatus(staleSource, TransactionStatus::RECEIVED));

        // CREATED -> FAILED is normally valid, but this copy is stale.
        assert(!engine.updateStatus(staleCopy, TransactionStatus::FAILED));
        assert(staleSource.status == TransactionStatus::RECEIVED);

        // Test 22: Unknown transaction is rejected
        Transaction unknownTransaction;

        unknownTransaction.transactionId = "PB-UNKNOWN";
        unknownTransaction.externalReference = "POS-UNKNOWN-001";
        unknownTransaction.amountMinor = 1000;
        unknownTransaction.currency = "AED";
        unknownTransaction.status = TransactionStatus::CREATED;

        assert(!engine.updateStatus(
            unknownTransaction,
            TransactionStatus::SENT
        ));
    }

    // Test 14: Transaction recovery after engine restart
    std::string savedTransactionId;

    {
        TransactionEngine firstEngine(testDatabasePath, std::string{});

        Transaction transaction =
            firstEngine.createTransaction(25000, "AED", "POS-RESTART-001");

        assert(transaction.status == TransactionStatus::CREATED);

        savedTransactionId = transaction.transactionId;

        assert(firstEngine.updateStatus(transaction, TransactionStatus::SENT));
        assert(firstEngine.updateStatus(transaction, TransactionStatus::RECEIVED));
        assert(firstEngine.updateStatus(transaction, TransactionStatus::APPROVED));
    }

    // The first engine has now been destroyed.
    {
        TransactionEngine restartedEngine(testDatabasePath, std::string{});

        Transaction recovered =
            restartedEngine.createTransaction(
                25000, "AED", "POS-RESTART-001"
            );

        assert(recovered.transactionId == savedTransactionId);
        assert(recovered.status == TransactionStatus::APPROVED);
    }

    // Tests 15 and 16: Persistence failure handling
    {
        const std::string blockedDatabase = blockerPath + "/transactions.sqlite";

        // Create a regular file where SQLite needs a parent directory.
        std::ofstream blocker(blockerPath);
        assert(blocker.is_open());
        blocker.close();

        // Test 15: failed database initialization must not register a transaction.
        TransactionEngine reliabilityEngine(blockedDatabase, std::string{});

        Transaction failedCreation =
            reliabilityEngine.createTransaction(
                3000, "AED", "POS-LOG-FAIL-001"
            );

        assert(failedCreation.status == TransactionStatus::FAILED);

        // Remove the blocker and retry. Failed initialization must be retryable.
        std::filesystem::remove(blockerPath);

        Transaction retryCreation =
            reliabilityEngine.createTransaction(
                3000, "AED", "POS-LOG-FAIL-001"
            );

        assert(retryCreation.status == TransactionStatus::CREATED);
        assert(!retryCreation.transactionId.empty());

        // Test 16: failed atomic status persistence must not change engine memory.
        sqlite3* failureDb = nullptr;
        assert(sqlite3_open(blockedDatabase.c_str(), &failureDb) == SQLITE_OK);
        assert(sqlite3_exec(failureDb,
            "CREATE TRIGGER reject_engine_sent BEFORE INSERT ON transaction_lifecycle_events "
            "WHEN NEW.to_status='SENT' BEGIN SELECT RAISE(ABORT,'injected'); END;",
            nullptr, nullptr, nullptr) == SQLITE_OK);
        sqlite3_close(failureDb);

        bool updateResult =
            reliabilityEngine.updateStatus(
                retryCreation,
                TransactionStatus::SENT
            );

        assert(!updateResult);
        assert(retryCreation.status == TransactionStatus::CREATED);

        assert(sqlite3_open(blockedDatabase.c_str(), &failureDb) == SQLITE_OK);
        assert(sqlite3_exec(failureDb, "DROP TRIGGER reject_engine_sent", nullptr, nullptr, nullptr) == SQLITE_OK);
        sqlite3_close(failureDb);

        assert(
            reliabilityEngine.updateStatus(
                retryCreation,
                TransactionStatus::SENT
            )
        );

        assert(retryCreation.status == TransactionStatus::SENT);

    }

    // Test 17: Concurrent duplicate transaction requests
    {
        TransactionEngine concurrentEngine(testDatabasePath, std::string{});

        const int threadCount = 20;
        const std::string externalReference = "POS-CONCURRENT-001";

        std::vector<Transaction> results(threadCount);
        std::vector<std::thread> workers;

        for (int i = 0; i < threadCount; ++i)
        {
            workers.emplace_back([&, i]()
            {
                results[i] = concurrentEngine.createTransaction(
                    5000,
                    "AED",
                    externalReference
                );
            });
        }

        // Wait for all threads to finish.
        for (auto& worker : workers)
        {
            worker.join();
        }

        // Every request must return the same transaction ID.
        const std::string firstTransactionId =
            results[0].transactionId;

        assert(!firstTransactionId.empty());

        for (const auto& result : results)
        {
            assert(result.status == TransactionStatus::CREATED);
            assert(result.transactionId == firstTransactionId);
        }

        // Verify only one transaction record was created in SQLite.
        sqlite3* concurrentDb = nullptr;
        assert(sqlite3_open(testDatabasePath.c_str(), &concurrentDb) == SQLITE_OK);
        sqlite3_stmt* concurrentCount = nullptr;
        assert(sqlite3_prepare_v2(concurrentDb,
            "SELECT count(*) FROM transactions WHERE external_reference=?",
            -1, &concurrentCount, nullptr) == SQLITE_OK);
        sqlite3_bind_text(concurrentCount, 1, externalReference.c_str(), -1, SQLITE_TRANSIENT);
        assert(sqlite3_step(concurrentCount) == SQLITE_ROW);
        assert(sqlite3_column_int(concurrentCount, 0) == 1);
        sqlite3_finalize(concurrentCount);
        sqlite3_close(concurrentDb);

        std::cout << "Concurrent duplicate request test passed!"
                  << std::endl;
    }

    // Test 23: CSV special characters and transaction recovery
    {
        TransactionLogger::setFilePath(testLog);

        Transaction specialTransaction;

        specialTransaction.transactionId = "PB-CSV-001";
        specialTransaction.externalReference =
            "POS-REF,\"SPECIAL\"\nSECOND-LINE";
        specialTransaction.amountMinor = 15000;
        specialTransaction.currency = "AED";

        // Supply complete asset metadata for the 12-column CSV format.
        specialTransaction.asset.type =
            paybridge::assets::AssetType::FIAT;
        specialTransaction.asset.code = "AED";
        specialTransaction.asset.decimalPrecision = 2;

        specialTransaction.money.units = 15000;
        specialTransaction.money.precision = 2;

        specialTransaction.status = TransactionStatus::CREATED;

        assert(TransactionLogger::logTransaction(specialTransaction));

        std::vector<Transaction> loaded =
            TransactionLogger::loadTransactions();

        bool foundSpecialTransaction = false;

        for (const auto& item : loaded)
        {
            if (item.transactionId == specialTransaction.transactionId)
            {
                assert(
                    item.externalReference ==
                    specialTransaction.externalReference
                );

                foundSpecialTransaction = true;
                break;
            }
        }

        assert(foundSpecialTransaction);

        std::cout
            << "CSV escaping and recovery test passed!"
            << std::endl;
    }

    // Test 24: Malformed CSV records must not affect valid records
    {
        const std::string malformedLog =
            (suiteRoot / "transaction_engine_malformed_tests.csv").string();

        TransactionLogger::setFilePath(malformedLog);

        std::filesystem::remove(malformedLog);

        {
            std::ofstream file(malformedLog, std::ios::binary);

            assert(file.is_open());

            file
                << "TransactionID,ExternalReference,AmountMinor,"
                << "Currency,Status,Timestamp,Environment\n";

            // Valid record 1
            file
                << "PB-VALID-001,POS-VALID-001,1000,"
                << "AED,CREATED,2026-11-24,SIMULATION\n";

            // Invalid record: only 5 columns
            file
                << "PB-BAD-001,POS-BAD-001,2000,AED,CREATED\n";

            // Invalid record: non-numeric amount
            file
                << "PB-BAD-002,POS-BAD-002,INVALID,"
                << "AED,CREATED,2026-11-24,SIMULATION\n";

            // Valid record 2 after malformed records
            file
                << "PB-VALID-002,POS-VALID-002,3000,"
                << "AED,APPROVED,2026-11-24,SIMULATION\n";
        }

        std::vector<Transaction> recovered =
            TransactionLogger::loadTransactions();

        assert(recovered.size() == 2);

        assert(recovered[0].transactionId == "PB-VALID-001");
        assert(recovered[0].amountMinor == 1000);

        assert(recovered[1].transactionId == "PB-VALID-002");
        assert(recovered[1].amountMinor == 3000);

        std::cout
            << "Malformed CSV recovery test passed!"
            << std::endl;

        TransactionLogger::setFilePath(testLog);

        std::filesystem::remove(malformedLog);
    }

    // Test 25: Incomplete quoted CSV record at EOF
    {
        const std::string incompleteLog =
            (suiteRoot / "transaction_engine_incomplete_tests.csv").string();

        TransactionLogger::setFilePath(incompleteLog);

        std::filesystem::remove(incompleteLog);

        {
            std::ofstream file(incompleteLog, std::ios::binary);

            assert(file.is_open());

            file
                << "TransactionID,ExternalReference,AmountMinor,"
                << "Currency,Status,Timestamp,Environment\n";

            // Valid transaction before the damaged record
            file
                << "PB-VALID-EOF-001,POS-VALID-EOF-001,5000,"
                << "AED,CREATED,2026-11-24,SIMULATION\n";

            // Damaged transaction: closing quotation mark is missing
            file
                << "PB-INCOMPLETE-001,\"POS-INCOMPLETE-001,6000,"
                << "AED,CREATED,2026-11-24,SIMULATION";
        }

        // Load transactions from the incomplete CSV file.
        std::vector<Transaction> recovered =
            TransactionLogger::loadTransactions();

        // Only the valid transaction should be recovered.
        assert(recovered.size() == 1);

        assert(
            recovered[0].transactionId == "PB-VALID-EOF-001"
        );

        assert(
            recovered[0].externalReference == "POS-VALID-EOF-001"
        );

        assert(recovered[0].amountMinor == 5000);
        assert(recovered[0].currency == "AED");

        assert(
            recovered[0].status == TransactionStatus::CREATED
        );

        std::cout
            << "Incomplete CSV EOF recovery test passed!"
            << std::endl;

        TransactionLogger::setFilePath(testLog);

        std::filesystem::remove(incompleteLog);
    }

    // P0 regression: existing RECEIVED/APPROVED/DECLINED transactions must not
    // attempt backward SENT/RECEIVED transitions when retried.
    {
        const std::string p0Log =
            (suiteRoot / "transaction_engine_p0_regression_tests.csv").string();

        // Isolate this test from application and previous test history.
        TransactionLogger::setFilePath(p0Log);
        std::filesystem::remove(p0Log);

        {
            TransactionEngine engine(testDatabasePath, std::string{});

            Transaction received =
                engine.createTransaction(7000, "AED", "POS-P0-RECEIVED");

            assert(received.status == TransactionStatus::CREATED);
            assert(engine.updateStatus(received, TransactionStatus::SENT));
            assert(engine.updateStatus(received, TransactionStatus::RECEIVED));

            Transaction duplicateReceived =
                engine.createTransaction(7000, "AED", "POS-P0-RECEIVED");

            assert(duplicateReceived.status == TransactionStatus::RECEIVED);
            assert(!engine.updateStatus(duplicateReceived, TransactionStatus::SENT));

            Transaction approved =
                engine.createTransaction(8000, "AED", "POS-P0-APPROVED");

            assert(approved.status == TransactionStatus::CREATED);
            assert(engine.updateStatus(approved, TransactionStatus::SENT));
            assert(engine.updateStatus(approved, TransactionStatus::RECEIVED));
            assert(engine.updateStatus(approved, TransactionStatus::APPROVED));

            Transaction duplicateApproved =
                engine.createTransaction(8000, "AED", "POS-P0-APPROVED");

            assert(duplicateApproved.status == TransactionStatus::APPROVED);
            assert(!engine.updateStatus(duplicateApproved, TransactionStatus::RECEIVED));

            Transaction declined =
                engine.createTransaction(9000, "AED", "POS-P0-DECLINED");

            assert(declined.status == TransactionStatus::CREATED);
            assert(engine.updateStatus(declined, TransactionStatus::SENT));
            assert(engine.updateStatus(declined, TransactionStatus::RECEIVED));
            assert(engine.updateStatus(declined, TransactionStatus::DECLINED));

            Transaction duplicateDeclined =
                engine.createTransaction(9000, "AED", "POS-P0-DECLINED");

            assert(duplicateDeclined.status == TransactionStatus::DECLINED);
            assert(!engine.updateStatus(duplicateDeclined, TransactionStatus::RECEIVED));
        }

        // Remove only the dedicated P0 test log.
        std::filesystem::remove(p0Log);
        TransactionLogger::resetFilePath();
    }
    // Restore the normal application log path.
    TransactionLogger::resetFilePath();

    std::cout << "All TransactionEngine tests passed!" << std::endl;
    std::cout << "Duplicate transaction protection tests passed!" << std::endl;
    std::cout << "Transaction restart recovery test passed!" << std::endl;
    std::cout << "Transaction logging failure tests passed!" << std::endl;

    // Terminal transport foundation regressions.
    {
        using namespace paybridge::terminal;
        using namespace paybridge::transport;

        SimulatedTerminalRequest simulatedRequest;
        simulatedRequest.requestId = "0123456789abcdef0123456789abcdef";
        simulatedRequest.transactionId = "PB-SIMULATED-001";
        simulatedRequest.externalReference = "POS-SIM-001";
        simulatedRequest.terminalId = "SIM-TERMINAL-001";
        simulatedRequest.amount = "12.34";
        simulatedRequest.assetIdentity = "FIAT:AED";
        const std::vector<std::uint8_t> request =
            SimulatedTerminalProtocol::encodeRequest(simulatedRequest);

        SimulatedTerminalTransport approved(SimulatedTerminalOutcome::APPROVED);
        assert(approved.open().success);
        assert(approved.send(request).success);

        TransportBytes approvedResponse = approved.receive(1024);
        assert(approvedResponse.result.success);

        SimulatedTerminalProtocol protocol;
        assert(protocol.validateRequest(simulatedRequest, request));
        assert(protocol.validateResponse(approvedResponse.data, simulatedRequest));

        SimulatedTerminalResponse parsed =
            protocol.parseResponse(approvedResponse.data, simulatedRequest);
        assert(parsed.outcome == SimulatedTerminalOutcome::APPROVED);
        assert(parsed.simulatedResult.find("SIMULATED") != std::string::npos);
        assert(parsed.requestId == simulatedRequest.requestId);
        assert(parsed.transactionId == simulatedRequest.transactionId);
        assert(parsed.externalReference == simulatedRequest.externalReference);
        assert(parsed.terminalId == simulatedRequest.terminalId);
        assert(parsed.simulated);

        assert(approvedResponse.data.size() >= 4);
        const std::vector<std::uint8_t> responseHeader(
            approvedResponse.data.begin(), approvedResponse.data.begin() + 4);
        const auto responseLength =
            SimulatedTerminalProtocol::decodeFrameLength(responseHeader);
        assert(responseLength.has_value());
        assert(approvedResponse.data.size() == 4 + *responseLength);

        const std::vector<std::uint8_t> responsePayload(
            approvedResponse.data.begin() + 4, approvedResponse.data.end());
        auto mismatchedResponse = nlohmann::json::parse(
            responsePayload.begin(), responsePayload.end());

        for (const auto& field : {"request_id", "transaction_id", "external_reference", "terminal_id"})
        {
            auto tampered = mismatchedResponse;
            tampered[field] = "MISMATCH";
            const std::string text = tampered.dump();
            const std::vector<std::uint8_t> payload(text.begin(), text.end());
            const auto framedBytes = SimulatedTerminalProtocol::framePayload(payload);
            assert(!protocol.validateResponse(framedBytes, simulatedRequest));
            assert(protocol.parseResponse(framedBytes, simulatedRequest).outcome ==
                SimulatedTerminalOutcome::MALFORMED);
        }
        approved.close();
        assert(!approved.isOpen());

        SimulatedTerminalTransport declined(SimulatedTerminalOutcome::DECLINED);
        assert(declined.open().success);
        assert(declined.send(request).success);
        TransportBytes declinedResponse = declined.receive(1024);
        assert(declinedResponse.result.success);
        assert(protocol.validateResponse(declinedResponse.data, simulatedRequest));
        parsed = protocol.parseResponse(declinedResponse.data, simulatedRequest);
        assert(parsed.outcome == SimulatedTerminalOutcome::DECLINED);
        declined.close();

        SimulatedTerminalTransport timeout(SimulatedTerminalOutcome::TIMEOUT);
        timeout.setReceiveTimeoutMilliseconds(50);
        assert(timeout.open().success);
        assert(timeout.send(request).success);
        TransportBytes timeoutResponse = timeout.receive(1024);
        assert(!timeoutResponse.result.success);
        assert(timeoutResponse.result.error == TransportError::TIMEOUT);
        timeout.close();

        SimulatedTerminalTransport disconnected(SimulatedTerminalOutcome::DISCONNECTED);
        assert(!disconnected.open().success);

        SimulatedTerminalTransport malformed(SimulatedTerminalOutcome::MALFORMED);
        assert(malformed.open().success);
        assert(malformed.send(request).success);
        TransportBytes malformedResponse = malformed.receive(1024);
        assert(malformedResponse.result.success);
        assert(!protocol.validateResponse(malformedResponse.data, simulatedRequest));
        parsed = protocol.parseResponse(malformedResponse.data, simulatedRequest);
        assert(parsed.outcome == SimulatedTerminalOutcome::MALFORMED);
        malformed.close();

        auto invalidRequest = simulatedRequest;
        invalidRequest.terminalId.clear();
        assert(!protocol.validateRequest(invalidRequest, request));
        assert(!protocol.validateRequest(simulatedRequest, {}));

        TerminalManager snapshotManager;
        assert(snapshotManager.terminalSnapshots().empty());
        TerminalModel disconnectedSimulator;
        disconnectedSimulator.terminalId = "SNAPSHOT-SIM-DISCONNECTED";
        disconnectedSimulator.transportKind = TransportKind::SIMULATED;
        snapshotManager.registerTerminal(disconnectedSimulator);
        snapshotManager.setTransport(disconnectedSimulator.terminalId,
            std::make_unique<SimulatedTerminalTransport>(SimulatedTerminalOutcome::APPROVED));
        TerminalModel connectingSimulator = disconnectedSimulator;
        connectingSimulator.terminalId = "SNAPSHOT-SIM-CONNECTING";
        connectingSimulator.connectionStatus = TerminalConnectionStatus::CONNECTING;
        snapshotManager.registerTerminal(connectingSimulator);
        snapshotManager.setTransport(connectingSimulator.terminalId,
            std::make_unique<SimulatedTerminalTransport>(SimulatedTerminalOutcome::APPROVED));
        TerminalModel unknownSimulator = disconnectedSimulator;
        unknownSimulator.terminalId = "SNAPSHOT-SIM-UNKNOWN";
        unknownSimulator.connectionStatus = TerminalConnectionStatus::UNKNOWN;
        snapshotManager.registerTerminal(unknownSimulator);
        snapshotManager.setTransport(unknownSimulator.terminalId,
            std::make_unique<SimulatedTerminalTransport>(SimulatedTerminalOutcome::APPROVED));
        TerminalModel failedSimulator = disconnectedSimulator;
        failedSimulator.terminalId = "SNAPSHOT-SIM-FAILED";
        failedSimulator.connectionStatus = TerminalConnectionStatus::FAILED;
        snapshotManager.registerTerminal(failedSimulator);
        snapshotManager.setTransport(failedSimulator.terminalId,
            std::make_unique<SimulatedTerminalTransport>(SimulatedTerminalOutcome::APPROVED));
        TerminalModel unconfiguredSimulator = disconnectedSimulator;
        unconfiguredSimulator.terminalId = "SNAPSHOT-SIM-UNCONFIGURED";
        snapshotManager.registerTerminal(unconfiguredSimulator);
        TerminalModel nonSimulator = disconnectedSimulator;
        nonSimulator.terminalId = "SNAPSHOT-NON-SIMULATED";
        nonSimulator.transportKind = TransportKind::TCP;
        snapshotManager.registerTerminal(nonSimulator);
        snapshotManager.setTransport(nonSimulator.terminalId,
            std::make_unique<SimulatedTerminalTransport>(SimulatedTerminalOutcome::APPROVED));

        const auto terminalSnapshots = snapshotManager.terminalSnapshots();
        assert(terminalSnapshots.size() == 6);
        for (std::size_t index = 1; index < terminalSnapshots.size(); ++index)
            assert(terminalSnapshots[index - 1].terminalId < terminalSnapshots[index].terminalId);
        auto snapshotFor = [&](const std::string& id) -> const TerminalReadModel& {
            for (const auto& snapshot : terminalSnapshots)
                if (snapshot.terminalId == id) return snapshot;
            assert(false);
            return terminalSnapshots.front();
        };
        const auto& disconnectedSnapshot = snapshotFor("SNAPSHOT-SIM-DISCONNECTED");
        assert(disconnectedSnapshot.simulated && disconnectedSnapshot.configured);
        assert(disconnectedSnapshot.connectionStatus == "DISCONNECTED");
        assert(disconnectedSnapshot.available); // Simulator can connect when the operation begins.
        assert(snapshotFor("SNAPSHOT-SIM-CONNECTING").configured &&
               !snapshotFor("SNAPSHOT-SIM-CONNECTING").available);
        assert(snapshotFor("SNAPSHOT-SIM-UNKNOWN").connectionStatus == "UNKNOWN" &&
               !snapshotFor("SNAPSHOT-SIM-UNKNOWN").available);
        assert(snapshotFor("SNAPSHOT-SIM-FAILED").connectionStatus == "FAILED" &&
               !snapshotFor("SNAPSHOT-SIM-FAILED").available);
        assert(!snapshotFor("SNAPSHOT-SIM-UNCONFIGURED").configured &&
               !snapshotFor("SNAPSHOT-SIM-UNCONFIGURED").available);
        assert(!snapshotFor("SNAPSHOT-NON-SIMULATED").simulated &&
               snapshotFor("SNAPSHOT-NON-SIMULATED").configured &&
               !snapshotFor("SNAPSHOT-NON-SIMULATED").available);
        assert(snapshotFor("SNAPSHOT-SIM-DISCONNECTED").connectionStatus == "DISCONNECTED");
        assert(snapshotManager.connect("SNAPSHOT-SIM-DISCONNECTED").success);
        const auto connectedSnapshot = snapshotManager.terminalSnapshots().front();
        assert(connectedSnapshot.terminalId == "SNAPSHOT-NON-SIMULATED"); // Stable ID ordering.
        const auto afterConnect = snapshotManager.terminalSnapshots();
        for (const auto& snapshot : afterConnect)
            if (snapshot.terminalId == "SNAPSHOT-SIM-DISCONNECTED")
                assert(snapshot.connectionStatus == "CONNECTED" && snapshot.available);

        TerminalManager terminalManager;

        TerminalModel first;
        first.terminalId = "VX680-DEMO-001";
        first.transportKind = TransportKind::SIMULATED;

        TerminalModel second;
        second.terminalId = "VX680-DEMO-002";
        second.transportKind = TransportKind::SIMULATED;

        terminalManager.registerTerminal(first);
        terminalManager.registerTerminal(second);

        assert(terminalManager.containsTerminal("VX680-DEMO-001"));
        assert(terminalManager.containsTerminal("VX680-DEMO-002"));
        assert(terminalManager.terminals().size() == 2);

        terminalManager.setTransport(
            "VX680-DEMO-001",
            std::make_unique<SimulatedTerminalTransport>(
                SimulatedTerminalOutcome::APPROVED
            )
        );

        std::string selectedTerminal;
        std::string selectionError;
        assert(terminalManager.selectSimulatedTerminal("", selectedTerminal, selectionError));
        assert(selectedTerminal == "VX680-DEMO-001");
        assert(terminalManager.selectSimulatedTerminal(
            "VX680-DEMO-001", selectedTerminal, selectionError));
        terminalManager.setTransport(
            "VX680-DEMO-002",
            std::make_unique<SimulatedTerminalTransport>(SimulatedTerminalOutcome::DECLINED));
        assert(!terminalManager.selectSimulatedTerminal("", selectedTerminal, selectionError));
        assert(selectionError == "Terminal selection is ambiguous");
        assert(terminalManager.selectSimulatedTerminal(
            "VX680-DEMO-002", selectedTerminal, selectionError));
        assert(selectedTerminal == "VX680-DEMO-002");

        assert(terminalManager.connect("VX680-DEMO-001").success);

        TransportBytes managerResponse = terminalManager.sendAndReceive(
            "VX680-DEMO-001",
            request,
            1024
        );
        assert(managerResponse.result.success);
        assert(protocol.validateResponse(managerResponse.data, simulatedRequest));

        terminalManager.disconnect("VX680-DEMO-001");
        assert(!terminalManager.sendAndReceive(
            "VX680-DEMO-001",
            request,
            128
        ).result.success);

        assert(terminalManager.removeTerminal("VX680-DEMO-001"));
        assert(!terminalManager.containsTerminal("VX680-DEMO-001"));

        TcpTransport tcpInvalid("not-an-ipv4-address", 5050);
        assert(tcpInvalid.open().error == TransportError::INVALID_ARGUMENT);
        assert(!tcpInvalid.isOpen());

        SerialTransport serialInvalid("COM_DOES_NOT_EXIST", 115200);
        assert(!serialInvalid.open().success);
        assert(!serialInvalid.isOpen());

        TransportBytes tcpReceiveClosed = tcpInvalid.receive(128);
        assert(!tcpReceiveClosed.result.success);
        assert(tcpReceiveClosed.result.error == TransportError::NOT_OPEN);

        TransportBytes serialReceiveClosed = serialInvalid.receive(128);
        assert(!serialReceiveClosed.result.success);
        assert(serialReceiveClosed.result.error == TransportError::NOT_OPEN);

        std::cout << "Terminal transport foundation tests passed!" << std::endl;
    }

    // Orchestration service simulator workflow smoke and replay check.
    {
        using namespace paybridge::terminal;
        const std::string workflowDb = (suiteRoot / "workflow.sqlite").string();
        TransactionEngine workflowEngine(workflowDb, std::string{});
        assert(workflowEngine.initialize());
        const auto aed = workflowEngine.assets().findFiat("AED");
        assert(aed);
        TerminalManager terminals;
        TerminalModel terminal;
        terminal.terminalId = "SIM-WORKFLOW-001";
        terminal.transportKind = TransportKind::SIMULATED;
        terminals.registerTerminal(terminal);
        terminals.registerAdapter(std::make_shared<SimulatedTerminalAdapter>());
        auto sendCount = std::make_shared<std::atomic<int>>(0);
        terminals.setTransport(terminal.terminalId,
            std::make_unique<CountingSimulatedTransport>(SimulatedTerminalOutcome::APPROVED, sendCount));
        PaymentOrchestrationService service(workflowEngine, terminals);
        PaymentRequest request;
        request.envelope.requestId = "2123456789abcdef0123456789abcdef";
        request.envelope.requestTimestamp = 1760000000;
        request.envelope.requestExpiry = 1760000300;
        request.externalReference = "SIM-WORKFLOW-REF-001";
        request.asset = *aed;
        request.money = {1234, aed->decimalPrecision};
        request.canonicalRequestHash = "canonical-workflow-test";
        request.paymentFingerprint = "fingerprint-workflow-test";
        const auto result = service.process(request);
        assert(result.status == "APPROVED");
        assert(result.resultCode == "SIMULATED_APPROVED");
        assert(result.simulated);
        assert(result.terminalId == terminal.terminalId);
        const auto retry = service.process(request);
        assert(retry.status == "APPROVED");
        assert(*sendCount == 1);
        assert(retry.transactionId == result.transactionId);
        assert(retry.message.find("SIMULATED APPROVED") != std::string::npos);
    }

    // End-to-end orchestration outcomes and durable restart safety.
    {
        using namespace paybridge::terminal;
        const auto runOutcome = [&](const std::string& suffix, SimulatedTerminalOutcome outcome,
                                    const std::string& expectedStatus)
        {
            const std::string dbPath = (suiteRoot / ("workflow_" + suffix + ".sqlite")).string();
            TransactionEngine workflowEngine(dbPath, std::string{});
            assert(workflowEngine.initialize());
            const auto aed = workflowEngine.assets().findFiat("AED");
            assert(aed);
            TerminalManager terminals;
            TerminalModel terminal;
            terminal.terminalId = "SIM-" + suffix;
            terminal.transportKind = TransportKind::SIMULATED;
            terminals.registerTerminal(terminal);
            terminals.registerAdapter(std::make_shared<SimulatedTerminalAdapter>());
            auto sendCount = std::make_shared<std::atomic<int>>(0);
            terminals.setTransport(terminal.terminalId,
                std::make_unique<CountingSimulatedTransport>(outcome, sendCount));
            PaymentOrchestrationService service(workflowEngine, terminals);
            PaymentRequest request;
            static std::size_t outcomeRequestNumber = 1;
            constexpr char HEX[] = "0123456789abcdef";
            std::size_t requestNumber = outcomeRequestNumber++;
            request.envelope.requestId.assign(32, '0');
            for (std::size_t index = request.envelope.requestId.size(); index > 0 && requestNumber; --index)
            {
                request.envelope.requestId[index - 1] = HEX[requestNumber & 0x0f];
                requestNumber >>= 4;
            }
            request.envelope.requestTimestamp = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            request.envelope.requestExpiry = request.envelope.requestTimestamp + 300;
            request.externalReference = "SIM-OUTCOME-" + suffix;
            request.asset = *aed;
            request.money = {500, aed->decimalPrecision};
            request.canonicalRequestHash = "canonical-" + suffix;
            request.paymentFingerprint = "fingerprint-" + suffix;
            const auto result = service.process(request);
            if (result.status != expectedStatus)
                std::cerr << "Simulated outcome " << suffix << " returned " << result.status
                          << " (" << result.resultCode << "): " << result.message << "\n";
            assert(result.status == expectedStatus);
            assert(result.simulated);
            if (expectedStatus == "APPROVED" || expectedStatus == "DECLINED")
                assert(result.message.find("no real payment was processed") != std::string::npos);
            if (expectedStatus == "UNKNOWN")
                assert(result.message.find("automatic resend is disabled") != std::string::npos);

            const auto replay = service.process(request);
            assert(replay.status == expectedStatus);
            assert(*sendCount == (expectedStatus == "FAILED" ? 0 : 1));
            assert(replay.transactionId == result.transactionId);
            assert(replay.terminalId == result.terminalId);
            if (expectedStatus == "APPROVED")
                assert(replay.message.find("SIMULATED APPROVED") != std::string::npos);
        };
        runOutcome("APPROVE", SimulatedTerminalOutcome::APPROVED, "APPROVED");
        runOutcome("DECLINE", SimulatedTerminalOutcome::DECLINED, "DECLINED");
        runOutcome("TIMEOUT", SimulatedTerminalOutcome::TIMEOUT, "UNKNOWN");
        runOutcome("MALFORMED", SimulatedTerminalOutcome::MALFORMED, "UNKNOWN");
        runOutcome("DISCONNECT", SimulatedTerminalOutcome::DISCONNECTED, "FAILED");

        const std::string recoveryDb = (suiteRoot / "workflow_recovery.sqlite").string();
        const std::string requestId = "3123456789abcdef0123456789abcdef";
        RequestReplayData replayData;
        replayData.requestId = requestId;
        replayData.canonicalRequestHash = "recovery-canonical";
        replayData.paymentFingerprint = "recovery-fingerprint";
        replayData.requestTimestamp = 1;
        replayData.requestExpiry = 2;
        std::string transactionId;
        {
            TransactionEngine engine(recoveryDb, std::string{});
            assert(engine.initialize());
            const auto aed = engine.assets().findFiat("AED");
            assert(aed);
            const auto registered = engine.createTransaction(
                paybridge::money::Money{700, aed->decimalPrecision}, *aed,
                "SIM-RECOVERY-REF", replayData);
            assert(registered.shouldProcess);
            Transaction transaction = registered.transaction;
            transactionId = transaction.transactionId;
            assert(engine.bindSimulatedTerminal(requestId, "SIM-RECOVERY", "PAYBRIDGE_SIMULATED_TERMINAL_V1"));
            assert(engine.updateStatusForRequest(transaction, TransactionStatus::SENT,
                requestId, ReplayState::PROCESSING, "SENT", "SIMULATED_REQUEST_SENT", "sent"));
        }
        {
            TransactionEngine recovered(recoveryDb, std::string{});
            assert(recovered.initialize());
            const auto aed = recovered.assets().findFiat("AED");
            assert(aed);
            const auto duplicate = recovered.createTransaction(
                paybridge::money::Money{700, aed->decimalPrecision}, *aed,
                "SIM-RECOVERY-REF", replayData);
            assert(duplicate.code == ReplayRegistrationCode::IDENTICAL_RETRY);
            assert(!duplicate.shouldProcess);
            assert(duplicate.replayState == ReplayState::UNKNOWN);
            assert(duplicate.transaction.transactionId == transactionId);
            assert(duplicate.transaction.status == TransactionStatus::UNKNOWN);
            assert(duplicate.terminalId == "SIM-RECOVERY");
        }
    }

    // A persistence failure AFTER the terminal was contacted must be reported
    // as UNKNOWN (never REJECTED) and must never cause a resend. Failures are
    // injected with a SQLite trigger installed from a second connection, so no
    // production test seam is needed.
    {
        using namespace paybridge::terminal;
        struct PostSendScenario
        {
            const char* name;
            const char* blockedStates;          // SQL list the trigger rejects
            TransactionStatus expectedStored;   // authoritative row afterwards
            const char* expectedReplayStatus;
        };
        const PostSendScenario scenarios[] = {
            {"receipt write fails",            "'RECEIVED'",           TransactionStatus::UNKNOWN,  "UNKNOWN"},
            {"final write fails",              "'APPROVED'",           TransactionStatus::UNKNOWN,  "UNKNOWN"},
            {"receipt and UNKNOWN writes fail","'RECEIVED','UNKNOWN'", TransactionStatus::SENT,     "PROCESSING"},
            {"final and UNKNOWN writes fail",  "'APPROVED','UNKNOWN'", TransactionStatus::RECEIVED, "PROCESSING"},
        };

        std::size_t scenarioNumber = 0;
        for (const auto& scenario : scenarios)
        {
            ++scenarioNumber;
            const std::string dbPath = (suiteRoot /
                ("post_send_persistence_" + std::to_string(scenarioNumber) + ".sqlite")).string();
            TransactionEngine engine(dbPath, std::string{});
            const bool engineReady = engine.initialize();
            assert(engineReady);
            (void)engineReady;

            sqlite3* injector = nullptr;
            const int openResult = sqlite3_open(dbPath.c_str(), &injector);
            assert(openResult == SQLITE_OK);
            (void)openResult;
            const std::string triggerSql =
                std::string("CREATE TRIGGER test_block_states BEFORE UPDATE ON transactions "
                            "WHEN NEW.lifecycle_state IN (") + scenario.blockedStates +
                ") BEGIN SELECT RAISE(ABORT, 'injected persistence failure'); END;";
            char* triggerError = nullptr;
            const int triggerResult = sqlite3_exec(injector, triggerSql.c_str(),
                nullptr, nullptr, &triggerError);
            if (triggerError)
            {
                std::cerr << "Trigger install failed (" << scenario.name << "): "
                          << triggerError << "\n";
                sqlite3_free(triggerError);
            }
            assert(triggerResult == SQLITE_OK);
            (void)triggerResult;
            sqlite3_close(injector);

            const auto aed = engine.assets().findFiat("AED");
            assert(aed);

            TerminalManager terminals;
            TerminalModel terminal;
            terminal.terminalId = "SIM-POST-SEND-" + std::to_string(scenarioNumber);
            terminal.transportKind = TransportKind::SIMULATED;
            terminals.registerTerminal(terminal);
            terminals.registerAdapter(std::make_shared<SimulatedTerminalAdapter>());
            auto sendCount = std::make_shared<std::atomic<int>>(0);
            terminals.setTransport(terminal.terminalId,
                std::make_unique<CountingSimulatedTransport>(
                    SimulatedTerminalOutcome::APPROVED, sendCount));
            PaymentOrchestrationService service(engine, terminals);

            PaymentRequest request;
            request.envelope.requestId.assign(32, '0');
            request.envelope.requestId[0] = '5';
            request.envelope.requestId[31] = static_cast<char>('0' + scenarioNumber);
            request.envelope.requestTimestamp = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            request.envelope.requestExpiry = request.envelope.requestTimestamp + 300;
            request.externalReference = "SIM-POST-SEND-REF-" + std::to_string(scenarioNumber);
            request.asset = *aed;
            request.money = {500, aed->decimalPrecision};
            request.canonicalRequestHash = "canonical-post-send-" + std::to_string(scenarioNumber);
            request.paymentFingerprint = "fingerprint-post-send-" + std::to_string(scenarioNumber);

            const auto result = service.process(request);
            if (result.status != "UNKNOWN")
                std::cerr << "Post-send scenario '" << scenario.name << "' returned "
                          << result.status << " (" << result.errorCode << ")\n";
            assert(result.status == "UNKNOWN");
            assert(result.errorCode == "PERSISTENCE_FAILURE_AFTER_SEND");
            assert(result.message.find("automatic resend is disabled") != std::string::npos);
            assert(result.simulated);
            assert(*sendCount == 1);

            const auto stored = engine.findTransactionById(result.transactionId);
            assert(stored.has_value());
            assert(stored->status == scenario.expectedStored);

            const auto replay = service.process(request);
            assert(replay.status == scenario.expectedReplayStatus);
            assert(replay.transactionId == result.transactionId);
            assert(*sendCount == 1);
        }
    }

    // Existing v1 SQLite databases upgrade in place and retain transaction/replay rows.
    {
        const std::string migrationDb = (suiteRoot / "schema_v1_upgrade.sqlite").string();
        RequestReplayData replayData;
        replayData.requestId = "4123456789abcdef0123456789abcdef";
        replayData.canonicalRequestHash = "schema-canonical";
        replayData.paymentFingerprint = "schema-fingerprint";
        replayData.requestTimestamp = 1760000000;
        replayData.requestExpiry = 1760000300;
        std::string originalId;
        {
            TransactionEngine engine(migrationDb, std::string{});
            assert(engine.initialize());
            const auto aed = engine.assets().findFiat("AED");
            assert(aed);
            const auto registered = engine.createTransaction(
                paybridge::money::Money{900, aed->decimalPrecision}, *aed,
                "SCHEMA-UPGRADE-REF", replayData);
            assert(registered.shouldProcess);
            originalId = registered.transaction.transactionId;
        }
        sqlite3* db = nullptr;
        assert(sqlite3_open(migrationDb.c_str(), &db) == SQLITE_OK);
        assert(sqlite3_exec(db,
            "ALTER TABLE replay_records DROP COLUMN simulation_contract;"
            "ALTER TABLE replay_records DROP COLUMN terminal_id;"
            "UPDATE schema_metadata SET metadata_value='1' WHERE metadata_key='schema_version';",
            nullptr, nullptr, nullptr) == SQLITE_OK);
        assert(sqlite3_close(db) == SQLITE_OK);
        {
            TransactionEngine upgraded(migrationDb, std::string{});
            assert(upgraded.initialize());
            const auto aed = upgraded.assets().findFiat("AED");
            assert(aed);
            const auto retry = upgraded.createTransaction(
                paybridge::money::Money{900, aed->decimalPrecision}, *aed,
                "SCHEMA-UPGRADE-REF", replayData);
            assert(retry.code == ReplayRegistrationCode::IDENTICAL_RETRY);
            assert(retry.transaction.transactionId == originalId);
            assert(retry.terminalId.empty());
        }
        assert(sqlite3_open(migrationDb.c_str(), &db) == SQLITE_OK);
        sqlite3_stmt* version = nullptr;
        assert(sqlite3_prepare_v2(db,
            "SELECT metadata_value FROM schema_metadata WHERE metadata_key='schema_version'",
            -1, &version, nullptr) == SQLITE_OK);
        assert(sqlite3_step(version) == SQLITE_ROW);
        assert(std::string(reinterpret_cast<const char*>(sqlite3_column_text(version, 0))) == "3");
        sqlite3_finalize(version);
        sqlite3_close(db);
    }

    const std::string authLog =
        (suiteRoot / ("transaction_engine_auth_tests_" + testToken + ".csv")).string();
    TransactionLogger::setFilePath(authLog);

    {
        TransactionEngine authEngine(testDatabasePath, std::string{});
        CommunicationManager communication(authEngine, validAuth);

        const std::int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        nlohmann::json paymentJson = {
            {"protocol_version", "1.0"},
            {"message_type", "PAYMENT_REQUEST"},
            {"external_reference", "POS-AUTH-001"},
            {"request_id", "0123456789abcdef0123456789abcdef"},
            {"request_timestamp", now},
            {"request_expiry", now + 300},
            {"payment", {
                {"amount", "10.00"},
                {"asset", {{"type", "FIAT"}, {"code", "AED"}}}
            }}
        };

        const std::string payment = paymentJson.dump();

        nlohmann::json validLf = communication.handleRequest(
            "Authorization: Bearer " + testToken + "\n" + payment);
        assert(validLf.value("status", "") == "APPROVED");

        const nlohmann::json identicalReplay = communication.handleRequest(
            "Authorization: Bearer " + testToken + "\n" + payment);
        assert(identicalReplay.value("status", "") == "APPROVED");
        assert(identicalReplay.value("transaction_id", "") == validLf.value("transaction_id", ""));

        nlohmann::json alteredReplay = paymentJson;
        alteredReplay["payment"]["amount"] = "11.00";
        const nlohmann::json conflictingReplay = communication.handleRequest(
            "Authorization: Bearer " + testToken + "\n" + alteredReplay.dump());
        assert(conflictingReplay.value("error_code", "") == "REPLAY_CONFLICT");


        std::string paymentCrlf = payment;
        const std::string lfReference = "POS-AUTH-001";
        const std::string crlfReference = "POS-AUTH-002";
        const std::size_t referencePosition = paymentCrlf.find(lfReference);
        assert(referencePosition != std::string::npos);
        paymentCrlf.replace(referencePosition, lfReference.size(), crlfReference);
        const std::string originalRequestId = "0123456789abcdef0123456789abcdef";
        const std::string secondRequestId = "1123456789abcdef0123456789abcdef";
        const std::size_t requestIdPosition = paymentCrlf.find(originalRequestId);
        assert(requestIdPosition != std::string::npos);
        paymentCrlf.replace(requestIdPosition, originalRequestId.size(), secondRequestId);

        nlohmann::json validCrlf = communication.handleRequest(
            "Authorization: Bearer " + testToken + "\r\n" + paymentCrlf);
        assert(validCrlf.value("status", "") == "APPROVED");

        nlohmann::json missing = communication.handleRequest(payment);
        assert(missing.value("error_code", "") == "AUTH_MISSING");

        nlohmann::json invalid = communication.handleRequest(
            "Authorization: Bearer " + invalidToken + "\n" + payment);
        assert(invalid.value("error_code", "") == "AUTH_INVALID");

        nlohmann::json malformed = communication.handleRequest(
            "Authorization:Basic " + testToken + "\n" + payment);
        assert(malformed.value("error_code", "") == "AUTH_MALFORMED");

        nlohmann::json duplicate = communication.handleRequest(
            "Authorization: Bearer " + testToken + " Authorization: Bearer " + testToken + "\n" + payment);
        assert(duplicate.value("error_code", "") == "AUTH_MALFORMED");

        nlohmann::json legacy = communication.handleRequest(payment);
        assert(legacy.value("error_code", "") == "AUTH_MISSING");

        nlohmann::json health = communication.handleRequest(
            "HEALTH_REQUEST JSON");
        assert(health.size() >= 0);

        nlohmann::json healthJson = communication.handleRequest(
            "{\"message_type\":\"HEALTH_REQUEST\"}");
        assert(healthJson.value("message_type", "") == "HEALTH_RESPONSE");
        assert(healthJson.value("status", "") == "OK");
        assert(healthJson.value("sensitive", true) == false);

        nlohmann::json healthWithPayment = communication.handleRequest(
            "Authorization: Bearer " + testToken + "\n" +
            "{\"message_type\":\"HEALTH_REQUEST\",\"payment\":" + payment + "}");
        assert(healthWithPayment.value("message_type", "") == "HEALTH_RESPONSE");

        nlohmann::json missingEnvelope = paymentJson;
        missingEnvelope.erase("request_id");

        nlohmann::json malformedEnvelope = paymentJson;
        malformedEnvelope["request_id"] = "bad-id";

        nlohmann::json missingTimestampEnvelope = paymentJson;
        missingTimestampEnvelope.erase("request_timestamp");

        nlohmann::json malformedTimestampEnvelope = paymentJson;
        malformedTimestampEnvelope["request_timestamp"] = "not-a-timestamp";

        nlohmann::json negativeTimestampEnvelope = paymentJson;
        negativeTimestampEnvelope["request_timestamp"] = -1;

        nlohmann::json expiryBeforeEnvelope = paymentJson;
        expiryBeforeEnvelope["request_expiry"] = now - 1;

        nlohmann::json expiryEqualEnvelope = paymentJson;
        expiryEqualEnvelope["request_expiry"] = now;

        nlohmann::json expiredEnvelope = paymentJson;
        expiredEnvelope["request_id"] = "1123456789abcdef0123456789abcdef";
        expiredEnvelope["request_timestamp"] = now - 100;
        expiredEnvelope["request_expiry"] = now - 1;

        nlohmann::json excessiveEnvelope = paymentJson;
        excessiveEnvelope["request_id"] = "2123456789abcdef0123456789abcdef";
        excessiveEnvelope["request_expiry"] = now + 301;

        nlohmann::json futureEnvelope = paymentJson;
        futureEnvelope["request_id"] = "3123456789abcdef0123456789abcdef";
        futureEnvelope["request_timestamp"] = now + 305;
        futureEnvelope["request_expiry"] = now + 306;

        nlohmann::json pastEnvelope = paymentJson;
        pastEnvelope["request_id"] = "4123456789abcdef0123456789abcdef";
        pastEnvelope["request_timestamp"] = now - 301;
        pastEnvelope["request_expiry"] = now - 300;

        nlohmann::json int64MaxEnvelope = paymentJson;
        int64MaxEnvelope["request_id"] = "5123456789abcdef0123456789abcdef";
        int64MaxEnvelope["request_timestamp"] = std::numeric_limits<std::int64_t>::max();

        nlohmann::json int64MinEnvelope = paymentJson;
        int64MinEnvelope["request_id"] = "6123456789abcdef0123456789abcdef";
        int64MinEnvelope["request_timestamp"] = std::numeric_limits<std::int64_t>::min();

        nlohmann::json unsignedOverflowEnvelope = paymentJson;
        unsignedOverflowEnvelope["request_id"] = "7123456789abcdef0123456789abcdef";
        unsignedOverflowEnvelope["request_timestamp"] = std::string("18446744073709551615");

        paybridge::assets::AssetRegistry envelopeRegistry;

        PaymentValidation validEnvelopeResult =
            JsonHandler::parseAndValidatePaymentRequest(paymentJson.dump(), envelopeRegistry);
        assert(validEnvelopeResult.valid);

        PaymentValidation missingEnvelopeResult =
            JsonHandler::parseAndValidatePaymentRequest(missingEnvelope.dump(), envelopeRegistry);
        assert(!missingEnvelopeResult.valid);
        assert(missingEnvelopeResult.errorCode == "INVALID_REQUEST_ID");

        PaymentValidation malformedEnvelopeResult =
            JsonHandler::parseAndValidatePaymentRequest(malformedEnvelope.dump(), envelopeRegistry);
        assert(!malformedEnvelopeResult.valid);
        assert(malformedEnvelopeResult.errorCode == "INVALID_REQUEST_ID");

        PaymentValidation missingTimestampResult =
            JsonHandler::parseAndValidatePaymentRequest(missingTimestampEnvelope.dump(), envelopeRegistry);
        assert(!missingTimestampResult.valid);
        assert(missingTimestampResult.errorCode == "INVALID_REQUEST_TIMESTAMP");

        PaymentValidation malformedTimestampResult =
            JsonHandler::parseAndValidatePaymentRequest(malformedTimestampEnvelope.dump(), envelopeRegistry);
        assert(!malformedTimestampResult.valid);
        assert(malformedTimestampResult.errorCode == "INVALID_REQUEST_TIMESTAMP");

        PaymentValidation negativeTimestampResult =
            JsonHandler::parseAndValidatePaymentRequest(negativeTimestampEnvelope.dump(), envelopeRegistry);
        assert(!negativeTimestampResult.valid);
        assert(negativeTimestampResult.errorCode == "INVALID_REQUEST_TIMESTAMP");

        PaymentValidation expiryBeforeResult =
            JsonHandler::parseAndValidatePaymentRequest(expiryBeforeEnvelope.dump(), envelopeRegistry);
        assert(!expiryBeforeResult.valid);
        assert(expiryBeforeResult.errorCode == "INVALID_REQUEST_EXPIRY");

        PaymentValidation expiryEqualResult =
            JsonHandler::parseAndValidatePaymentRequest(expiryEqualEnvelope.dump(), envelopeRegistry);
        assert(!expiryEqualResult.valid);
        assert(expiryEqualResult.errorCode == "INVALID_REQUEST_EXPIRY");

        PaymentValidation expiredEnvelopeResult =
            JsonHandler::parseAndValidatePaymentRequest(expiredEnvelope.dump(), envelopeRegistry);
        assert(!expiredEnvelopeResult.valid);
        assert(expiredEnvelopeResult.errorCode == "REQUEST_EXPIRED");

        PaymentValidation excessiveEnvelopeResult =
            JsonHandler::parseAndValidatePaymentRequest(excessiveEnvelope.dump(), envelopeRegistry);
        assert(!excessiveEnvelopeResult.valid);
        assert(excessiveEnvelopeResult.errorCode == "REQUEST_LIFETIME_EXCEEDED");

        PaymentValidation futureEnvelopeResult =
            JsonHandler::parseAndValidatePaymentRequest(futureEnvelope.dump(), envelopeRegistry);
        assert(!futureEnvelopeResult.valid);
        assert(futureEnvelopeResult.errorCode == "REQUEST_TIMESTAMP_INVALID");

        PaymentValidation pastEnvelopeResult =
            JsonHandler::parseAndValidatePaymentRequest(pastEnvelope.dump(), envelopeRegistry);
        assert(!pastEnvelopeResult.valid);
        assert(pastEnvelopeResult.errorCode == "REQUEST_TIMESTAMP_INVALID");

        PaymentValidation int64MaxResult =
            JsonHandler::parseAndValidatePaymentRequest(int64MaxEnvelope.dump(), envelopeRegistry);
        assert(!int64MaxResult.valid);
        assert(int64MaxResult.errorCode == "REQUEST_LIFETIME_EXCEEDED" ||
               int64MaxResult.errorCode == "REQUEST_TIMESTAMP_INVALID" ||
               int64MaxResult.errorCode == "INVALID_REQUEST_EXPIRY");

        PaymentValidation int64MinResult =
            JsonHandler::parseAndValidatePaymentRequest(int64MinEnvelope.dump(), envelopeRegistry);
        assert(!int64MinResult.valid);
        assert(int64MinResult.errorCode == "INVALID_REQUEST_TIMESTAMP");

        unsignedOverflowEnvelope["request_timestamp"] = 18446744073709551615ULL;
        PaymentValidation unsignedOverflowResult =
            JsonHandler::parseAndValidatePaymentRequest(unsignedOverflowEnvelope.dump(), envelopeRegistry);
        assert(!unsignedOverflowResult.valid);
        assert(unsignedOverflowResult.errorCode == "INVALID_REQUEST_TIMESTAMP");

        nlohmann::json missingExpiryEnvelope = paymentJson;
        missingExpiryEnvelope.erase("request_expiry");

        nlohmann::json malformedExpiryEnvelope = paymentJson;
        malformedExpiryEnvelope["request_expiry"] = "not-a-timestamp";

        PaymentValidation missingExpiryResult =
            JsonHandler::parseAndValidatePaymentRequest(missingExpiryEnvelope.dump(), envelopeRegistry);
        assert(!missingExpiryResult.valid);
        assert(missingExpiryResult.errorCode == "INVALID_REQUEST_EXPIRY");

        PaymentValidation malformedExpiryResult =
            JsonHandler::parseAndValidatePaymentRequest(malformedExpiryEnvelope.dump(), envelopeRegistry);
        assert(!malformedExpiryResult.valid);
        assert(malformedExpiryResult.errorCode == "INVALID_REQUEST_EXPIRY");
    }

    std::cout << "All AuthManager tests passed!" << std::endl;

    return 0;
}
