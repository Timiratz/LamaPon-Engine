#include "LamaPon/Online/NetworkTransport.h"

#if defined(LAMAPON_WITH_EOS)
#include "LamaPon/Online/EpicNetworkTransportSdk.h"
#endif

namespace LamaPon
{
    bool HasEpicNetworkBackend() noexcept
    {
#if defined(LAMAPON_WITH_EOS)
        return true;
#else
        return false;
#endif
    }
}

namespace LamaPon::Detail
{
    std::unique_ptr<INetworkTransport> CreateEpicTransport()
    {
#if defined(LAMAPON_WITH_EOS)
        return CreateEpicSdkTransport();
#else
        return {};
#endif
    }
}
