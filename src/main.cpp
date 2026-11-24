#include <iostream>
#include <cstdlib>

#include "communication/CommunicationManager.h"

int main()
{
    std::cout << "\n====================================\n";
    std::cout << "          PAYBRIDGE SYSTEM          \n";
    std::cout << "====================================\n";

    TransactionEngine engine;
    CommunicationManager communication(engine);

    const char* usdtNetwork = std::getenv("PAYBRIDGE_USDT_NETWORK");
    const char* usdtContract = std::getenv("PAYBRIDGE_USDT_CONTRACT_ADDRESS");
    const char* usdtPrecision = std::getenv("PAYBRIDGE_USDT_PRECISION");

    if (usdtNetwork && usdtContract && usdtPrecision)
    {
        try
        {
            const unsigned int precision = std::stoul(usdtPrecision);
            if (precision <= 18)
            {
                engine.assets().registerUsdt(
                    usdtNetwork,
                    usdtContract,
                    static_cast<std::uint8_t>(precision)
                );
            }
        }
        catch (...) {}
    }

    std::cout << "[READY] Communication Manager\n";

    return communication.runServer(5050);
}
