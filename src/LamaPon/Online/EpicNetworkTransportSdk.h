#pragma once

#include "LamaPon/Online/NetworkTransport.h"

namespace LamaPon::Detail
{
    // EOS SDKによる通信実装を所有権付きで作る。
    std::unique_ptr<INetworkTransport> CreateEpicSdkTransport();
}
