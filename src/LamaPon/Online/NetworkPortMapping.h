#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace LamaPon::Detail
{
    struct NetworkPortEntry final
    {
        std::string client;
        std::uint16_t port{};
        std::string description;
        std::uint32_t leaseSeconds{};
        bool enabled{ true };
    };
    class INetworkGateway
    {
    public:
        virtual ~INetworkGateway() = default;
        // 通信エラーは例外、未登録だけをnulloptで表します。
        virtual std::optional<NetworkPortEntry> Inspect(std::uint16_t port) = 0;
        virtual bool Add(std::uint16_t externalPort, const NetworkPortEntry& entry) = 0;
        virtual void Remove(std::uint16_t externalPort) = 0;
        virtual std::string ExternalAddress() = 0;
    };
    class NetworkPortLease final
    {
    public:
        NetworkPortLease(INetworkGateway& gateway, std::string client, std::uint16_t port,
            std::string description);
        ~NetworkPortLease();
        bool Acquire();
        bool Renew();
        void Release() noexcept;
    private:
        bool Owns(const NetworkPortEntry& entry) const;
        INetworkGateway& m_gateway;
        NetworkPortEntry m_entry;
        bool m_owned{};
    };
    class NetworkPortMapping final
    {
    public:
        NetworkPortMapping();
        ~NetworkPortMapping();
        void Start(const std::string& listenAddress);
        void Stop() noexcept;
        std::string Endpoint() const;
        std::string Status() const;
    private:
        struct Implementation;
        std::shared_ptr<Implementation> m_impl;
    };
    bool IsPublicNetworkIpv4(const std::string& address);
}
