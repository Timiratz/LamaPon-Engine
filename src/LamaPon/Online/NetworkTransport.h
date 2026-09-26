#pragma once

#include "LamaPon/Online/NetworkSession.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace LamaPon::Detail
{
    inline constexpr std::size_t NetworkPacketMaxBytes = 1100;
    inline constexpr std::size_t NetworkObjectLimit = 128;
    inline constexpr std::size_t NetworkEventLimit = 512;

    using TransportPeer = std::uint64_t;
    enum class TransportEventKind { Ready, Connected, Disconnected, Message, Error };
    struct TransportEvent final
    {
        TransportEventKind kind{};
        TransportPeer peer{};
        std::string data;
    };

    // 両方のバックエンドは、接続単位で順序付き・信頼性ありのパケットを
    // 配信します。受信・送信キューと1回のPollの作業量には上限があります。
    class INetworkTransport
    {
    public:
        virtual ~INetworkTransport() = default;
        virtual bool Start(const NetworkConfiguration& configuration,
            bool host, const std::string& address, const std::string& name) = 0;
        virtual void Stop() noexcept = 0;
        virtual std::vector<TransportEvent> Poll(float elapsedSeconds) = 0;
        virtual bool Send(TransportPeer peer, const std::string& packet) = 0;
        virtual void Disconnect(TransportPeer peer) = 0;
        virtual std::string Address() const = 0;
        virtual std::string Error() const = 0;
        virtual std::string AccessKey() const { return {}; }
        virtual std::string ConnectionCode(const std::string&) const { return Address(); }
        virtual std::string Status() const { return {}; }
        virtual std::string LocalAddress() const { return Address(); }
    };

    std::unique_ptr<INetworkTransport> CreateLanTransport();
    std::unique_ptr<INetworkTransport> CreateEpicTransport();
    std::unique_ptr<INetworkTransport> CreateDirectTransport();
}
