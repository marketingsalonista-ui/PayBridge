#include <iostream>

#include "communication/CommunicationManager.h"

int main()
{
    std::cout << "\n====================================\n";
    std::cout << "          PAYBRIDGE SYSTEM          \n";
    std::cout << "====================================\n";

    CommunicationManager communication;

    std::cout << "[READY] Communication Manager\n";

    return communication.runServer(5050);
}
