#include "FiatAdapter.h"

namespace paybridge::adapters
{

bool FiatAdapter::supports(const assets::Asset& asset) const
{
    return asset.isFiat();
}

PaymentAdapterResult FiatAdapter::simulate(
    const assets::Asset&,
    const money::Money& amount,
    const std::string& externalReference) const
{
    PaymentAdapterResult result;

    if (!amount.isPositive() || externalReference.empty())
    {
        result.status = "REJECTED";
        result.message = "Invalid fiat simulator request";
        return result;
    }

    result.accepted = true;
    result.status = "RECEIVED";
    result.message = "Request received by PayBridge fiat simulator";
    return result;
}

} // namespace paybridge::adapters
