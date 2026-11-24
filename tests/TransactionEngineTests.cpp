
#include "../src/core/TransactionEngine.h"
#include "../src/core/TransactionLogger.h"

#include <iostream>
#include <cassert>
#include <filesystem>

int main()
{
    const std::string testLog = "transaction_engine_tests.csv";

    // Use a separate log so production transaction history is untouched.
    TransactionLogger::setFilePath(testLog);
    std::filesystem::remove(testLog);

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

    // Restore the normal application log path.
    TransactionLogger::resetFilePath();

    std::cout << "All TransactionEngine tests passed!" << std::endl;
    std::cout << "Duplicate transaction protection tests passed!" << std::endl;
    std::cout << "Transaction restart recovery test passed!" << std::endl;

    return 0;
}
