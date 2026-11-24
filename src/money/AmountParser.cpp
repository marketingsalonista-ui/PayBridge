#include "AmountParser.h"

#include <limits>

namespace paybridge::money
{

bool ParseAmountResult::success() const
{
    return error == ParseAmountError::NONE;
}

ParseAmountResult parseAmount(
    const std::string& decimalText,
    std::uint8_t precision,
    std::int64_t maxUnits)
{
    ParseAmountResult result;

    if (decimalText.empty())
    {
        result.error = ParseAmountError::EMPTY;
        return result;
    }

    if (precision > 18)
    {
        result.error = ParseAmountError::UNSUPPORTED_PRECISION;
        return result;
    }

    if (decimalText.front() == '-')
    {
        result.error = ParseAmountError::NEGATIVE;
        return result;
    }

    if (decimalText.front() == '+')
    {
        result.error = ParseAmountError::MALFORMED;
        return result;
    }

    if (decimalText.find_first_not_of("0123456789.") != std::string::npos)
    {
        result.error = ParseAmountError::MALFORMED;
        return result;
    }

    const std::string::size_type dot = decimalText.find('.');
    if (dot != std::string::npos && decimalText.find('.', dot + 1) != std::string::npos)
    {
        result.error = ParseAmountError::MALFORMED;
        return result;
    }

    std::string whole = dot == std::string::npos
        ? decimalText
        : decimalText.substr(0, dot);

    std::string fraction = dot == std::string::npos
        ? std::string{}
        : decimalText.substr(dot + 1);

    if (whole.empty())
        whole = "0";

    if (fraction.size() > precision)
    {
        result.error = ParseAmountError::EXCESS_DECIMAL_PLACES;
        return result;
    }

    std::int64_t units = 0;

    for (char character : whole)
    {
        const std::int64_t digit = character - '0';
        if (units > maxUnits / 10)
        {
            result.error = ParseAmountError::OVERFLOW;
            return result;
        }
        units *= 10;
        if (units > maxUnits - digit)
        {
            result.error = ParseAmountError::OVERFLOW;
            return result;
        }
        units += digit;
    }

    for (std::uint8_t index = 0; index < precision; ++index)
    {
        const char digitCharacter = index < fraction.size()
            ? fraction[index]
            : '0';

        const std::int64_t digit = digitCharacter - '0';
        if (units > maxUnits / 10)
        {
            result.error = ParseAmountError::OVERFLOW;
            return result;
        }
        units *= 10;
        if (units > maxUnits - digit)
        {
            result.error = ParseAmountError::OVERFLOW;
            return result;
        }
        units += digit;
    }

    if (units == 0)
    {
        result.error = ParseAmountError::ZERO;
        return result;
    }

    result.money.units = units;
    result.money.precision = precision;
    result.error = ParseAmountError::NONE;
    return result;
}

std::string formatAmount(const Money& money)
{
    if (money.precision > 18)
        return {};

    if (money.units <= 0)
        return "0";

    std::string digits = std::to_string(money.units);

    if (money.precision == 0)
        return digits;

    while (digits.size() <= money.precision)
        digits.insert(digits.begin(), '0');

    digits.insert(digits.size() - money.precision, ".");
    return digits;
}

} // namespace paybridge::money
