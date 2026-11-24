#pragma once

#include <cstdint>
#include <string>

namespace paybridge::assets
{

enum class AssetType
{
    FIAT,
    DIGITAL_ASSET
};

struct Asset
{
    AssetType type = AssetType::FIAT;
    std::string code;
    std::string network;
    std::string contractAddress;
    std::uint8_t decimalPrecision = 0;

    bool isFiat() const;
    bool isDigitalAsset() const;
    bool isComplete() const;

    bool operator==(const Asset& other) const;
    bool operator!=(const Asset& other) const;

    std::string identityKey() const;
};

std::string assetTypeToString(AssetType type);

} // namespace paybridge::assets
