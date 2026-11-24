#pragma once

#include "../assets/Asset.h"
#include "../money/Money.h"

#include <string>

namespace paybridge::adapters
{

// Simulator-only adapter boundary. These methods must not imply bank,
// card, acquirer, processor, or blockchain integration.
struct PaymentAdapterResult
{
    bool accepted = false;
    std::string status;
    std::string message;
};

class PaymentAdapter
{
public:
    virtual ~PaymentAdapter() = default;
    virtual bool supports(const assets::Asset& asset) const = 0;
    virtual PaymentAdapterResult simulate(
        const assets::Asset& asset,
        const money::Money& amount,
        const std::string& externalReference
    ) const = 0;
};

} // namespace paybridge::adapters
