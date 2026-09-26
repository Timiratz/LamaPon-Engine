#pragma once

#include "LamaPon/Online/NetworkTransport.h"

namespace LamaPon::Detail
{
    std::unique_ptr<INetworkTransport> CreateEpicSdkTransport();
}
