#include "DigitalAssetAdapter.h"

namespace paybridge::adapters
{

bool DigitalAssetAdapter::supports(const assets::Asset& asset) const
{
    return asset.isDigitalAsset();
}

PaymentAdapterResult DigitalAssetAdapter::simulate(
    const assets::Asset&,
    const money::Money& amount,
    const std::string& externalReference) const
{
    PaymentAdapterResult result;

    if (!amount.isPositive() || externalReference.empty())
    {
        result.status = "REJECTED";
        result.message = "Invalid digital asset simulator request";
        return result;
    }

    result.accepted = true;
    result.status = "RECEIVED";
    result.message = "Request received by PayBridge digital asset simulator";
    return result;
}

} // namespace paybridge::adapters
