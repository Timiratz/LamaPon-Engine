#pragma once

#include "LamaPon/Online/NetworkRoomDirectory.h"
#include <memory>

namespace LamaPon
{
    enum class NetworkDiscoveryScope { Lan, SameComputer };
    // LAN検索はローカルUDPだけを使い、インターネットのサービスへ問い合わせません。
    class NetworkRoomBrowser final
    {
    public:
        LAMAPON_API NetworkRoomBrowser();
        LAMAPON_API ~NetworkRoomBrowser();
        NetworkRoomBrowser(const NetworkRoomBrowser&) = delete;
        NetworkRoomBrowser& operator=(const NetworkRoomBrowser&) = delete;
        LAMAPON_API bool Start(NetworkConfiguration configuration,
            NetworkDiscoveryScope scope = NetworkDiscoveryScope::Lan);
        LAMAPON_API void Refresh();
        LAMAPON_API void Update(float seconds);
        LAMAPON_API void Stop();
        [[nodiscard]] LAMAPON_API bool IsSearching() const noexcept;
        [[nodiscard]] LAMAPON_API const std::vector<NetworkRoom>& Rooms() const noexcept;
        [[nodiscard]] LAMAPON_API const std::string& LastError() const noexcept;
    private:
        struct Implementation;
        std::unique_ptr<Implementation> m_impl;
    };
}
