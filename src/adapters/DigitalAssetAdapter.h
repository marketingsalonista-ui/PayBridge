#pragma once

#include "PaymentAdapter.h"

namespace paybridge::adapters
{

class DigitalAssetAdapter : public PaymentAdapter
{
public:
    bool supports(const assets::Asset& asset) const override;

    PaymentAdapterResult simulate(
        const assets::Asset& asset,
        const money::Money& amount,
        const std::string& externalReference
    ) const override;
};

} // namespace paybridge::adapters
