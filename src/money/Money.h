#pragma once

#include <cstdint>
#include <string>

namespace paybridge::money
{

// Exact integer units at the asset's configured decimal precision.
// For example, GBP 125.50 with precision 2 is stored as 12550.
// The supported precision is 0 through 18; the maximum number of units is INT64_MAX.
struct Money
{
    std::int64_t units = 0;
    std::uint8_t precision = 0;

    bool isPositive() const;
};

} // namespace paybridge::money
