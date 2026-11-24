#include "Money.h"

namespace paybridge::money
{

bool Money::isPositive() const
{
    return units > 0 && precision <= 18;
}

} // namespace paybridge::money
