#pragma once

#include "../core/TransactionEngine.h"

class CommunicationManager
{
public:
    explicit CommunicationManager(TransactionEngine& engine);
    int runServer(unsigned short port);

private:
    TransactionEngine& engine;
};
