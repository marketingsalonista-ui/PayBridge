#pragma once

#include "Money.h"

#include <cstdint>
#include <optional>
#include <string>

namespace paybridge::money
{

enum class ParseAmountError
{
    NONE,
    EMPTY,
    MALFORMED,
    NEGATIVE,
    ZERO,
    EXCESS_DECIMAL_PLACES,
    UNSUPPORTED_PRECISION,
    OVERFLOW
};

struct ParseAmountResult
{
    ParseAmountError error = ParseAmountError::NONE;
    Money money;

    bool success() const;
};

ParseAmountResult parseAmount(
    const std::string& decimalText,
    std::uint8_t precision,
    std::int64_t maxUnits = 9223372036854775807LL
);

std::string formatAmount(const Money& money);

} // namespace paybridge::money
