#include "TransactionLogger.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <mutex>
#include <vector>
#include <string>
#include <stdexcept>
#include <utility>

namespace
{
    const std::string defaultFilename = "logs/transactions.csv";

    std::string currentFilename = defaultFilename;

    // Protects logger operations and shared file-path access.
    std::mutex loggerMutex;

    enum class CsvReadResult
    {
        RECORD,
        END,
        MALFORMED
    };

    std::string getTimestamp()
    {
        auto now = std::chrono::system_clock::now();

        std::time_t currentTime =
            std::chrono::system_clock::to_time_t(now);

        std::tm localTime{};

#ifdef _WIN32
        localtime_s(&localTime, &currentTime);
#else
        localtime_r(&currentTime, &localTime);
#endif

        std::ostringstream timestamp;

        timestamp << std::put_time(
            &localTime,
            "%Y-%m-%d %H:%M:%S"
        );

        return timestamp.str();
    }

    TransactionStatus stringToStatus(const std::string& status)
    {
        if (status == "CREATED") return TransactionStatus::CREATED;
        if (status == "SENT") return TransactionStatus::SENT;
        if (status == "RECEIVED") return TransactionStatus::RECEIVED;
        if (status == "APPROVED") return TransactionStatus::APPROVED;
        if (status == "DECLINED") return TransactionStatus::DECLINED;
        if (status == "FAILED") return TransactionStatus::FAILED;
        if (status == "UNKNOWN") return TransactionStatus::UNKNOWN;

        return TransactionStatus::INVALID;
    }

    // Escape CSV fields containing commas, quotes or newlines.
    std::string escapeCsvField(const std::string& value)
    {
        bool needsQuotes =
            value.find(',') != std::string::npos ||
            value.find('"') != std::string::npos ||
            value.find('\n') != std::string::npos ||
            value.find('\r') != std::string::npos;

        if (!needsQuotes)
        {
            return value;
        }

        std::string escaped = "\"";

        for (char character : value)
        {
            if (character == '"')
            {
                escaped += "\"\"";
            }
            else
            {
                escaped += character;
            }
        }

        escaped += "\"";

        return escaped;
    }

    // Skip the remainder of a malformed physical line.
    void skipToNextLine(std::istream& input, std::string* rawRecord = nullptr)
    {
        char character;

        while (input.get(character))
        {
            if (character == '\n')
            {
                if (rawRecord)
                    rawRecord->push_back(character);
                break;
            }
            if (rawRecord)
                rawRecord->push_back(character);
        }
    }

    // Read one complete CSV record.
    // Newlines inside quoted fields are preserved.
    CsvReadResult readCsvRecord(
        std::istream& input,
        std::vector<std::string>& fields,
        std::string* rawRecord = nullptr)
    {
        fields.clear();

        std::string field;

        bool insideQuotes = false;
        bool afterClosingQuote = false;
        bool fieldStarted = false;
        bool sawCharacter = false;

        char character;

        while (input.get(character))
        {
            sawCharacter = true;
            if (rawRecord)
                rawRecord->push_back(character);

            if (insideQuotes)
            {
                if (character == '"')
                {
                    if (input.peek() == '"')
                    {
                        input.get(character);
                        if (rawRecord)
                            rawRecord->push_back(character);
                        field += '"';
                    }
                    else
                    {
                        insideQuotes = false;
                        afterClosingQuote = true;
                    }
                }
                else
                {
                    field += character;
                }

                continue;
            }

            if (afterClosingQuote)
            {
                if (character == ',')
                {
                    fields.push_back(field);
                    field.clear();

                    afterClosingQuote = false;
                    fieldStarted = false;
                }
                else if (character == '\n')
                {
                    fields.push_back(field);
                    return CsvReadResult::RECORD;
                }
                else if (character == '\r' && input.peek() == '\n')
                {
                    input.get(character);
                    if (rawRecord)
                        rawRecord->push_back(character);
                    fields.push_back(field);

                    return CsvReadResult::RECORD;
                }
                else
                {
                    skipToNextLine(input, rawRecord);
                    fields.clear();

                    return CsvReadResult::MALFORMED;
                }

                continue;
            }

            if (character == ',')
            {
                fields.push_back(field);
                field.clear();
                fieldStarted = false;
            }
            else if (character == '\n')
            {
                if (!field.empty() && field.back() == '\r')
                {
                    field.pop_back();
                }

                fields.push_back(field);

                return CsvReadResult::RECORD;
            }
            else if (character == '\r' && input.peek() == '\n')
            {
                input.get(character);
                if (rawRecord)
                    rawRecord->push_back(character);
                fields.push_back(field);

                return CsvReadResult::RECORD;
            }
            else if (character == '"')
            {
                if (fieldStarted || !field.empty())
                {
                    skipToNextLine(input, rawRecord);
                    fields.clear();

                    return CsvReadResult::MALFORMED;
                }

                insideQuotes = true;
                fieldStarted = true;
            }
            else
            {
                field += character;
                fieldStarted = true;
            }
        }

        if (insideQuotes)
        {
            fields.clear();
            return CsvReadResult::MALFORMED;
        }

        if (!sawCharacter)
        {
            return CsvReadResult::END;
        }

        if (!field.empty() && field.back() == '\r')
        {
            field.pop_back();
        }

        fields.push_back(field);

        return CsvReadResult::RECORD;
    }

    bool parseTransactionRecord(
        const std::vector<std::string>& fields,
        Transaction& transaction,
        std::string& diagnostic)
    {
        if (fields.size() != 7 && fields.size() != 12)
        {
            diagnostic = "Expected 7 or 12 columns, found " +
                         std::to_string(fields.size());
            return false;
        }

        transaction.transactionId = fields[0];
        transaction.externalReference = fields[1];
        transaction.currency = fields[3];
        if (transaction.transactionId.empty() || transaction.externalReference.empty())
        {
            diagnostic = "Transaction ID and external reference must be non-empty";
            return false;
        }

        try
        {
            std::size_t parsed = 0;
            transaction.amountMinor = std::stoll(fields[2], &parsed);
            if (parsed != fields[2].size() || transaction.amountMinor <= 0)
                throw std::invalid_argument("invalid amount");
        }
        catch (...)
        {
            diagnostic = "Amount is not a positive integer minor-unit value";
            return false;
        }

        transaction.status = stringToStatus(fields[4]);
        if (transaction.status == TransactionStatus::INVALID)
        {
            diagnostic = "Unrecognized or invalid transaction status";
            return false;
        }

        if (fields.size() == 7)
        {
            if (transaction.currency.size() != 3)
            {
                diagnostic = "Legacy fiat currency must contain three characters";
                return false;
            }
            transaction.asset.code = transaction.currency;
            transaction.asset.type = paybridge::assets::AssetType::FIAT;
            transaction.money.units = transaction.amountMinor;
            return true;
        }

        if (fields[7] == "FIAT")
            transaction.asset.type = paybridge::assets::AssetType::FIAT;
        else if (fields[7] == "DIGITAL_ASSET")
            transaction.asset.type = paybridge::assets::AssetType::DIGITAL_ASSET;
        else
        {
            diagnostic = "Unrecognized asset type";
            return false;
        }

        transaction.asset.code = fields[8];
        transaction.asset.network = fields[9];
        transaction.asset.contractAddress = fields[10];
        try
        {
            std::size_t parsed = 0;
            const unsigned long precision = std::stoul(fields[11], &parsed);
            if (parsed != fields[11].size() || precision > 18)
                throw std::invalid_argument("invalid precision");
            transaction.asset.decimalPrecision = static_cast<std::uint8_t>(precision);
            transaction.money.precision = static_cast<std::uint8_t>(precision);
            transaction.money.units = transaction.amountMinor;
        }
        catch (...)
        {
            diagnostic = "Decimal precision is invalid";
            return false;
        }

        if (!transaction.asset.isComplete())
        {
            diagnostic = "Asset identity is incomplete";
            return false;
        }
        if (transaction.asset.isFiat() && transaction.asset.code != transaction.currency)
        {
            diagnostic = "Fiat asset code does not match currency";
            return false;
        }
        if (transaction.asset.isFiat() &&
            (!transaction.asset.network.empty() || !transaction.asset.contractAddress.empty()))
        {
            diagnostic = "Fiat asset must not contain network or contract fields";
            return false;
        }
        return true;
    }
}

TransactionLogger::CsvReadReport TransactionLogger::readTransactionsFromFile(
    const std::string& path)
{
    CsvReadReport report;
    std::lock_guard<std::mutex> lock(loggerMutex);
    try
    {
        if (!std::filesystem::exists(path))
            return report;
        report.sourceExists = true;

        std::ifstream file(path, std::ios::binary);
        if (!file.is_open())
        {
            report.diagnostic = "Could not open CSV source";
            return report;
        }

        std::vector<std::string> fields;
        std::string rawHeader;
        const CsvReadResult headerResult = readCsvRecord(file, fields, &rawHeader);
        const std::vector<std::string> historicalHeader = {
            "Transaction ID", "Amount", "Currency", "Status",
            "Timestamp", "Environment"};
        const std::vector<std::string> legacyHeader = {
            "TransactionID", "ExternalReference", "AmountMinor", "Currency",
            "Status", "Timestamp", "Environment"};
        const std::vector<std::string> currentHeader = {
            "TransactionID", "ExternalReference", "AmountMinor", "Currency",
            "Status", "Timestamp", "Environment", "AssetType", "AssetCode",
            "AssetNetwork", "ContractAddress", "DecimalPrecision"};
        if (headerResult != CsvReadResult::RECORD ||
            (fields != historicalHeader && fields != legacyHeader && fields != currentHeader))
        {
            report.rawHeader = std::move(rawHeader);
            report.diagnostic = "CSV header is missing, malformed, or unsupported";
            return report;
        }
        report.rawHeader = std::move(rawHeader);
        report.headerValid = true;

        std::size_t rowNumber = 0;
        while (true)
        {
            CsvRecord record;
            record.rowNumber = ++rowNumber;
            const CsvReadResult readResult =
                readCsvRecord(file, fields, &record.rawRecord);
            if (readResult == CsvReadResult::END)
                break;

            if (readResult == CsvReadResult::MALFORMED)
            {
                record.diagnostic = "Malformed CSV quoting or record boundary";
            }
            else
            {
                record.columnCount = fields.size();
                if (fields.size() == 6)
                {
                    record.auditOnly = true;
                    record.diagnostic =
                        "Six-column historical record retained for audit; not imported";
                }
                else
                {
                    record.valid = parseTransactionRecord(
                        fields, record.transaction, record.diagnostic);
                }
            }
            report.records.push_back(std::move(record));
        }
    }
    catch (const std::exception& error)
    {
        report.diagnostic = error.what();
    }
    return report;
}

std::string TransactionLogger::filePath()
{
    std::lock_guard<std::mutex> lock(loggerMutex);
    return currentFilename;
}

void TransactionLogger::setFilePath(const std::string& path)
{
    std::lock_guard<std::mutex> lock(loggerMutex);

    if (!path.empty())
    {
        currentFilename = path;
    }
}

void TransactionLogger::resetFilePath()
{
    std::lock_guard<std::mutex> lock(loggerMutex);

    currentFilename = defaultFilename;
}

bool TransactionLogger::logTransaction(
    const Transaction& transaction)
{
    std::lock_guard<std::mutex> lock(loggerMutex);

    try
    {
        std::filesystem::path logPath(currentFilename);

        std::filesystem::path parent =
            logPath.parent_path();

        if (!parent.empty())
        {
            std::filesystem::create_directories(parent);
        }

        bool needsHeader =
            !std::filesystem::exists(currentFilename) ||
            std::filesystem::file_size(currentFilename) == 0;

        std::ofstream file(
            currentFilename,
            std::ios::app | std::ios::binary
        );

        if (!file.is_open())
        {
            std::cerr
                << "[ERROR] Could not open transaction log.\n";

            return false;
        }

        if (needsHeader)
        {
            file
                << "TransactionID,ExternalReference,AmountMinor,"
                << "Currency,Status,Timestamp,Environment,AssetType,AssetCode,"
                << "AssetNetwork,ContractAddress,DecimalPrecision\n";
        }

        file
            << escapeCsvField(transaction.transactionId) << ","
            << escapeCsvField(transaction.externalReference) << ","
            << transaction.amountMinor << ","
            << escapeCsvField(transaction.currency) << ","
            << escapeCsvField(
                TransactionEngine::statusToString(
                    transaction.status
                )
            ) << ","
            << escapeCsvField(getTimestamp()) << ","
            << escapeCsvField("SIMULATION") << ","
            << escapeCsvField(
                paybridge::assets::assetTypeToString(transaction.asset.type)
            ) << ","
            << escapeCsvField(transaction.asset.code) << ","
            << escapeCsvField(transaction.asset.network) << ","
            << escapeCsvField(transaction.asset.contractAddress) << ","
            << static_cast<unsigned int>(transaction.money.precision)
            << "\n";

        file.close();

        if (!file)
        {
            std::cerr
                << "[ERROR] Failed writing transaction log.\n";

            return false;
        }

        std::cout
            << "[LOGGED] Transaction saved to "
            << currentFilename << "\n";

        return true;
    }
    catch (const std::exception& error)
    {
        std::cerr
            << "[ERROR] Logging failed: "
            << error.what() << "\n";

        return false;
    }
}

std::vector<Transaction> TransactionLogger::loadTransactions()
{
    std::lock_guard<std::mutex> lock(loggerMutex);

    std::vector<Transaction> transactions;

    try
    {
        if (!std::filesystem::exists(currentFilename))
        {
            return transactions;
        }

        std::ifstream file(
            currentFilename,
            std::ios::binary
        );

        if (!file.is_open())
        {
            std::cerr
                << "[ERROR] Could not read transaction log.\n";

            return transactions;
        }

        std::vector<std::string> fields;

        // Read and validate the header.
        CsvReadResult headerResult =
            readCsvRecord(file, fields);

        if (headerResult != CsvReadResult::RECORD)
        {
            std::cerr
                << "[ERROR] Transaction log header is missing or malformed.\n";

            return transactions;
        }

        int recordNumber = 1;

        while (true)
        {
            CsvReadResult result =
                readCsvRecord(file, fields);

            if (result == CsvReadResult::END)
            {
                break;
            }

            ++recordNumber;

            if (result == CsvReadResult::MALFORMED)
            {
                std::cerr
                    << "[WARNING] Skipping malformed CSV record "
                    << recordNumber << ".\n";

                continue;
            }

            if (fields.size() != 7 && fields.size() != 12)
            {
                std::cerr
                    << "[WARNING] Skipping CSV record "
                    << recordNumber
                    << ": expected 7 or 12 columns, found "
                    << fields.size() << ".\n";

                continue;
            }

            Transaction transaction;

            transaction.transactionId = fields[0];
            transaction.externalReference = fields[1];
            transaction.currency = fields[3];

            const std::string& amount = fields[2];
            const std::string& status = fields[4];

            if (transaction.transactionId.empty() ||
                transaction.externalReference.empty())
            {
                std::cerr
                    << "[WARNING] Invalid transaction at CSV record "
                    << recordNumber << ".\n";

                continue;
            }

            if (fields.size() == 7)
            {
                if (transaction.currency.size() != 3)
                {
                    std::cerr
                        << "[WARNING] Invalid transaction at CSV record "
                        << recordNumber << ".\n";

                    continue;
                }
            }

            try
            {
                std::size_t parsed = 0;

                transaction.amountMinor =
                    std::stoll(amount, &parsed);

                if (parsed != amount.size() ||
                    transaction.amountMinor <= 0)
                {
                    throw std::invalid_argument("Invalid amount");
                }
            }
            catch (...)
            {
                std::cerr
                    << "[WARNING] Invalid amount at CSV record "
                    << recordNumber << ".\n";

                continue;
            }

            transaction.status = stringToStatus(status);

            if (fields.size() == 12)
            {
                bool validAssetType = false;

                if (fields[7] == "FIAT")
                {
                    transaction.asset.type = paybridge::assets::AssetType::FIAT;
                    validAssetType = true;

                    if (transaction.asset.code.empty())
                        transaction.asset.code = transaction.currency;
                }
                else if (fields[7] == "DIGITAL_ASSET")
                {
                    transaction.asset.type = paybridge::assets::AssetType::DIGITAL_ASSET;
                    validAssetType = true;
                }

                if (!validAssetType)
                    continue;

                transaction.asset.code = fields[8];
                transaction.asset.network = fields[9];
                transaction.asset.contractAddress = fields[10];

                try
                {
                    std::size_t parsedPrecision = 0;
                    const unsigned long precision =
                        std::stoul(fields[11], &parsedPrecision);

                    if (parsedPrecision != fields[11].size() || precision > 18)
                        throw std::invalid_argument("Invalid precision");

                    transaction.asset.decimalPrecision =
                        static_cast<std::uint8_t>(precision);
                    transaction.money.precision =
                        static_cast<std::uint8_t>(precision);
                    transaction.money.units = transaction.amountMinor;

                    if (!transaction.asset.isComplete())
                        continue;

                    if (transaction.asset.isFiat() &&
                        transaction.asset.code != transaction.currency)
                        continue;

                    if (transaction.asset.isFiat() &&
                        transaction.asset.network.empty() == false)
                        continue;

                    if (transaction.asset.isFiat() &&
                        transaction.asset.contractAddress.empty() == false)
                        continue;
                }
                catch (...)
                {
                    continue;
                }
            }

            if (transaction.status == TransactionStatus::INVALID)
            {
                std::cerr
                    << "[WARNING] Invalid status at CSV record "
                    << recordNumber << ".\n";

                continue;
            }

            transactions.push_back(transaction);
        }
    }
    catch (const std::exception& error)
    {
        std::cerr
            << "[ERROR] Loading transactions failed: "
            << error.what() << "\n";
    }

    return transactions;
}
