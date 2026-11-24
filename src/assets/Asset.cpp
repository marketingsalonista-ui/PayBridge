#include "Asset.h"

#include <utility>

namespace paybridge::assets
{

bool Asset::isFiat() const
{
    return type == AssetType::FIAT;
}

bool Asset::isDigitalAsset() const
{
    return type == AssetType::DIGITAL_ASSET;
}

bool Asset::isComplete() const
{
    if (code.empty())
        return false;

    if (type == AssetType::FIAT)
        return network.empty() && contractAddress.empty();

    return !network.empty() && !contractAddress.empty();
}

bool Asset::operator==(const Asset& other) const
{
    return type == other.type &&
           code == other.code &&
           network == other.network &&
           contractAddress == other.contractAddress &&
           decimalPrecision == other.decimalPrecision;
}

bool Asset::operator!=(const Asset& other) const
{
    return !(*this == other);
}

std::string Asset::identityKey() const
{
    if (type == AssetType::FIAT)
        return std::string("FIAT:") + code;

    return std::string("DIGITAL_ASSET:") + code + ":" + network + ":" + contractAddress;
}

std::string assetTypeToString(AssetType type)
{
    return type == AssetType::FIAT ? "FIAT" : "DIGITAL_ASSET";
}

} // namespace paybridge::assets
