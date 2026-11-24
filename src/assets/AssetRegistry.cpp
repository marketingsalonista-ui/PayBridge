#include "AssetRegistry.h"

#include <algorithm>
#include <cctype>

namespace
{
bool upperCaseAscii(const std::string& value)
{
    if (value.empty())
        return false;

    for (unsigned char character : value)
    {
        if (character < 'A' || character > 'Z')
            return false;
    }
    return true;
}

std::string trim(std::string value)
{
    auto isSpace = [](unsigned char character)
    {
        return std::isspace(character) != 0;
    };

    while (!value.empty() && isSpace(static_cast<unsigned char>(value.front())))
        value.erase(value.begin());

    while (!value.empty() && isSpace(static_cast<unsigned char>(value.back())))
        value.pop_back();

    return value;
}
}

namespace paybridge::assets
{

AssetRegistry::AssetRegistry()
{
    registerFiatCurrency("AED", 2);
    registerFiatCurrency("GBP", 2);
    registerFiatCurrency("USD", 2);
    registerFiatCurrency("EUR", 2);
    registerFiatCurrency("INR", 2);
    registerFiatCurrency("SAR", 2);
    registerFiatCurrency("JPY", 0);
    registerFiatCurrency("KWD", 3);
}

bool AssetRegistry::registerAsset(const Asset& asset)
{
    if (!asset.isComplete())
        return false;

    if (asset.isFiat())
    {
        if (!upperCaseAscii(asset.code) || asset.decimalPrecision > 18)
            return false;
    }
    else
    {
        if (!upperCaseAscii(asset.code) ||
            trim(asset.network).empty() ||
            trim(asset.contractAddress).empty() ||
            asset.decimalPrecision > 18)
            return false;
    }

    std::lock_guard<std::mutex> lock(mutex);

    const std::string key = asset.identityKey();
    const auto existing = assets.find(key);

    if (existing == assets.end())
    {
        assets.emplace(key, asset);
        return true;
    }

    return existing->second == asset;
}

bool AssetRegistry::registerFiatCurrency(const std::string& code, std::uint8_t precision)
{
    if (!upperCaseAscii(code) || precision > 18)
        return false;

    Asset asset;
    asset.type = AssetType::FIAT;
    asset.code = code;
    asset.decimalPrecision = precision;

    return registerAsset(asset);
}

bool AssetRegistry::registerUsdt(
    const std::string& network,
    const std::string& contractAddress,
    std::uint8_t decimalPrecision)
{
    if (trim(network).empty() || trim(contractAddress).empty() || decimalPrecision > 18)
        return false;

    Asset asset;
    asset.type = AssetType::DIGITAL_ASSET;
    asset.code = "USDT";
    asset.network = trim(network);
    asset.contractAddress = trim(contractAddress);
    asset.decimalPrecision = decimalPrecision;

    return registerAsset(asset);
}

bool AssetRegistry::contains(const Asset& asset) const
{
    std::lock_guard<std::mutex> lock(mutex);
    return assets.find(asset.identityKey()) != assets.end() &&
           assets.at(asset.identityKey()) == asset;
}

std::optional<Asset> AssetRegistry::findFiat(const std::string& code) const
{
    if (!upperCaseAscii(code))
        return std::nullopt;

    Asset lookup;
    lookup.type = AssetType::FIAT;
    lookup.code = code;

    const std::string key = lookup.identityKey();

    std::lock_guard<std::mutex> lock(mutex);
    const auto existing = assets.find(key);
    return existing == assets.end() ? std::nullopt : std::optional<Asset>(existing->second);
}

std::optional<Asset> AssetRegistry::resolveDigitalAsset(
    const std::string& code,
    const std::string& network,
    const std::string& contractAddress) const
{
    if (!upperCaseAscii(code) || trim(network).empty() || trim(contractAddress).empty())
        return std::nullopt;

    Asset lookup;
    lookup.type = AssetType::DIGITAL_ASSET;
    lookup.code = code;
    lookup.network = trim(network);
    lookup.contractAddress = trim(contractAddress);

    const std::string key = lookup.identityKey();

    std::lock_guard<std::mutex> lock(mutex);
    const auto existing = assets.find(key);

    if (existing == assets.end())
        return std::nullopt;

    lookup.decimalPrecision = existing->second.decimalPrecision;
    return lookup;
}

} // namespace paybridge::assets
