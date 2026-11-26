#pragma once

#include "Asset.h"

#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace paybridge::assets
{

struct AssetReadModel
{
    std::string code;
    std::string type;
    std::uint8_t precision = 0;
    // The registry currently stores enabled assets only.
    bool enabled = true;
};

class AssetRegistry
{
public:
    AssetRegistry();

    bool registerAsset(const Asset& asset);
    bool registerFiatCurrency(const std::string& code, std::uint8_t precision);

    // USDT is intentionally not self-registered by a client or by network guessing.
    // The caller must supply values from trusted deployment configuration.
    bool registerUsdt(
        const std::string& network,
        const std::string& contractAddress,
        std::uint8_t decimalPrecision
    );

    bool contains(const Asset& asset) const;
    std::vector<AssetReadModel> readModels() const;
    std::optional<Asset> findFiat(const std::string& code) const;
    std::optional<Asset> resolveDigitalAsset(
        const std::string& code,
        const std::string& network,
        const std::string& contractAddress
    ) const;

private:
    mutable std::mutex mutex;
    std::unordered_map<std::string, Asset> assets;
};

} // namespace paybridge::assets
