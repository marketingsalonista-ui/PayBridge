
#include "../src/core/TransactionEngine.h"
#include "../src/core/TransactionLogger.h"

#include <iostream>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>
#include <string>

int main()
{
    const std::string testLog = "transaction_engine_tests.csv";
    const std::string blockerPath = "transaction_engine_log_blocker";

    // Use a separate log so production transaction history is untouched.
    TransactionLogger::setFilePath(testLog);
    std::filesystem::remove(testLog);
    std::filesystem::remove_all(blockerPath);

    {
        TransactionEngine engine;

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
        TransactionEngine firstEngine;

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
        TransactionEngine restartedEngine;

        Transaction recovered =
            restartedEngine.createTransaction(
                25000, "AED", "POS-RESTART-001"
            );

        assert(recovered.transactionId == savedTransactionId);
        assert(recovered.status == TransactionStatus::APPROVED);
    }

    // Tests 15 and 16: Persistence failure handling
    {
        TransactionEngine reliabilityEngine;

        // Create a regular file where the log directory should be.
        // This deliberately makes log directory creation fail.
        std::ofstream blocker(blockerPath);
        assert(blocker.is_open());
        blocker.close();

        // Test 15: Failed logging must not register a new transaction.
        TransactionLogger::setFilePath(
            blockerPath + "/transactions.csv"
        );

        Transaction failedCreation =
            reliabilityEngine.createTransaction(
                3000, "AED", "POS-LOG-FAIL-001"
            );

        assert(failedCreation.status == TransactionStatus::FAILED);

        // Restore the test log and retry the same external reference.
        TransactionLogger::setFilePath(testLog);

        Transaction retryCreation =
            reliabilityEngine.createTransaction(
                3000, "AED", "POS-LOG-FAIL-001"
            );

        assert(retryCreation.status == TransactionStatus::CREATED);
        assert(!retryCreation.transactionId.empty());

        // Test 16: Failed status logging must not change transaction state.
        TransactionLogger::setFilePath(
            blockerPath + "/transactions.csv"
        );

        bool updateResult =
            reliabilityEngine.updateStatus(
                retryCreation,
                TransactionStatus::SENT
            );

        assert(!updateResult);
        assert(retryCreation.status == TransactionStatus::CREATED);

        // Restore logging and confirm the same update can now succeed.
        TransactionLogger::setFilePath(testLog);

        assert(
            reliabilityEngine.updateStatus(
                retryCreation,
                TransactionStatus::SENT
            )
        );

        assert(retryCreation.status == TransactionStatus::SENT);

        std::filesystem::remove(blockerPath);
    }

    // Test 17: Concurrent duplicate transaction requests
    {
        TransactionEngine concurrentEngine;

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

        // Verify only one transaction record was created.
        std::ifstream logFile(testLog);
        std::string line;
        int matchingRecords = 0;

        while (std::getline(logFile, line))
        {
            if (line.find(externalReference) != std::string::npos)
            {
                ++matchingRecords;
            }
        }

        assert(matchingRecords == 1);

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

        std::cout << "CSV escaping and recovery test passed!"
                  << std::endl;
    }

    // Restore the normal application log path.
    TransactionLogger::resetFilePath();

    std::cout << "All TransactionEngine tests passed!" << std::endl;
    std::cout << "Duplicate transaction protection tests passed!" << std::endl;
    std::cout << "Transaction restart recovery test passed!" << std::endl;
    std::cout << "Transaction logging failure tests passed!" << std::endl;

    return 0;
}
