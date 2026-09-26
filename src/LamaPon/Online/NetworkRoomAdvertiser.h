#pragma once

#include "LamaPon/Online/NetworkSession.h"
#include <memory>

namespace LamaPon::Detail
{
    class NetworkRoomAdvertiser final
    {
    public:
        NetworkRoomAdvertiser();
        ~NetworkRoomAdvertiser();
        bool Start(const NetworkConfiguration& configuration);
        void Update(const NetworkSession& session);
        void Stop() noexcept;
    private:
        struct Implementation;
        std::unique_ptr<Implementation> m_impl;
    };
}
