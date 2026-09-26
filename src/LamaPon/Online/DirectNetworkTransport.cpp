#include "LamaPon/Online/NetworkEndpoint.h"
#include "LamaPon/Online/NetworkTransport.h"
#include "LamaPon/Online/NetworkCrypto.h"
#include "LamaPon/Online/NetworkPortMapping.h"

#include <algorithm>
#include <map>
#include <stdexcept>

namespace LamaPon::Detail
{
    namespace
    {
        constexpr std::string_view Prefix = "LPD1|";
        constexpr std::string_view Hello = "LPDH1";
        constexpr std::size_t HelloSize = 5 + 32 + 72;
        constexpr std::string_view HostProof = "LamaPon.Direct.Host.1";
        constexpr std::string_view ClientProof = "LamaPon.Direct.Client.1";
        struct Wipe final
        {
            NetworkKey& key;
            ~Wipe() { SecureZeroMemory(key.data(), key.size()); }
        };
        struct DirectPeer final
        {
            std::unique_ptr<NetworkKeyExchange> exchange{ std::make_unique<NetworkKeyExchange>() };
            NetworkCipher send, receive;
            NetworkKey nonce{ RandomNetworkKey() };
            std::string hello;
            int stage{};
            float age{};
            DirectPeer()
            {
                hello = Hello;
                hello.append(reinterpret_cast<const char*>(nonce.data()), nonce.size());
                const auto key = exchange->PublicKey();
                hello.append(reinterpret_cast<const char*>(key.data()), key.size());
            }
            ~DirectPeer() { SecureZeroMemory(nonce.data(), nonce.size()); }
        };
        class DirectTransport final : public INetworkTransport
        {
        public:
            ~DirectTransport() override { Stop(); }
            bool Start(const NetworkConfiguration& configuration, const bool host,
                const std::string& address, const std::string& name) override
            {
                Stop(); m_error.clear(); m_host = host;
                std::string endpoint = address;
                if (host)
                {
                    try { m_roomKey = RandomNetworkKey(); }
                    catch (...) { m_error = "通信の秘密情報を作成できません。"; return false; }
                }
                else
                {
                    if (!address.starts_with(Prefix)) { m_error = "Direct接続には接続情報、または接続先とアクセスキーが必要です。"; return false; }
                    const auto split = address.find('|', Prefix.size());
                    if (split == std::string::npos || !Unhex(std::string_view(address).substr(split + 1), m_roomKey))
                    { m_error = "Directの接続情報が無効です。"; return false; }
                    endpoint = address.substr(Prefix.size(), split - Prefix.size());
                }
                m_inner = CreateLanTransport();
                if (!m_inner->Start(configuration, host, endpoint, name))
                { m_error = m_inner->Error(); Stop(); return false; }
                m_endpoint = m_inner->Address();
                if (host && configuration.automaticPortMapping) m_mapping.Start(m_endpoint);
                return true;
            }
            void Stop() noexcept override
            {
                m_mapping.Stop();
                if (m_inner) m_inner->Stop();
                m_inner.reset(); m_peers.clear(); m_endpoint.clear();
                SecureZeroMemory(m_roomKey.data(), m_roomKey.size());
            }
            std::vector<TransportEvent> Poll(const float seconds) override
            {
                std::vector<TransportEvent> result;
                if (!m_inner) return result;
                for (auto& [id, peer] : m_peers)
                { static_cast<void>(id); if (peer->stage != 2) peer->age += seconds; }
                for (const auto& event : m_inner->Poll(seconds))
                {
                    if (event.kind == TransportEventKind::Connected)
                    {
                        try
                        {
                            auto peer = std::make_unique<DirectPeer>();
                            if (!m_host && !m_inner->Send(event.peer, peer->hello)) throw std::runtime_error("Handshake queue");
                            m_peers.emplace(event.peer, std::move(peer));
                        }
                        catch (...) { Reject(event.peer, result); }
                    }
                    else if (event.kind == TransportEventKind::Message)
                    {
                        const auto found = m_peers.find(event.peer); if (found == m_peers.end()) continue;
                        try { Receive(event, *found->second, result); }
                        catch (...) { Reject(event.peer, result); }
                    }
                    else if (event.kind == TransportEventKind::Disconnected)
                    {
                        const auto found = m_peers.find(event.peer);
                        if (found != m_peers.end())
                        {
                            if (found->second->stage == 2) result.push_back(event);
                            else if (!m_host) AuthenticationError(result);
                            m_peers.erase(found);
                        }
                        else if (!m_host)
                        {
                            m_error = "接続先に到達できません。接続先・ポート転送・ファイアウォールを確認してください。";
                            result.push_back({ TransportEventKind::Error, 0, m_error });
                        }
                    }
                    else result.push_back(event);
                }
                std::vector<TransportPeer> expired;
                for (const auto& [id, peer] : m_peers) if (peer->stage != 2 && peer->age > 5) expired.push_back(id);
                for (const auto id : expired) Reject(id, result);
                return result;
            }
            bool Send(const TransportPeer id, const std::string& packet) override
            {
                const auto found = m_peers.find(id);
                if (!m_inner || found == m_peers.end() || found->second->stage != 2
                    || packet.empty() || packet.size() > NetworkPacketMaxBytes) return false;
                try { return m_inner->Send(id, found->second->send.Seal(packet)); }
                catch (...) { return false; }
            }
            void Disconnect(const TransportPeer id) override { if (m_inner) m_inner->Disconnect(id); }
            std::string Error() const override { return m_error; }
            std::string AccessKey() const override { return m_inner && m_host ? Hex(m_roomKey) : std::string{}; }
            std::string Address() const override { return ConnectionCode({}); }
            std::string LocalAddress() const override { return m_endpoint; }
            std::string ConnectionCode(const std::string& requested) const override
            {
                if (!m_inner) return {};
                NetworkEndpoint local;
                if (!NetworkEndpoint::Parse(m_endpoint, 0, local)) return {};
                std::string address = requested.empty() ? m_mapping.Endpoint() : requested;
                if (address.empty()) address = local.Wildcard()
                    ? (local.Family() == AF_INET ? "127.0.0.1:" : "[::1]:") + std::to_string(local.Port()) : local.Text();
                NetworkEndpoint endpoint;
                if (!NetworkEndpoint::Parse(address, local.Port(), endpoint) || !endpoint.Unicast() || endpoint.Port() == 0) return {};
                return std::string(Prefix) + endpoint.Text() + "|" + Hex(m_roomKey);
            }
            std::string Status() const override
            {
                const auto mapping = m_mapping.Status();
                return "Direct: 共有アクセスキーで認証、ECDH / AES-256-GCMで暗号化。待受 / 接続先: " + m_endpoint
                    + (mapping.empty() ? "。外部回線では公開接続先とポート転送、または到達可能なIPv6が必要です。" : "。" + mapping);
            }
        private:
            void Derive(DirectPeer& peer, const std::string_view remoteHello)
            {
                if (remoteHello.size() != HelloSize || !remoteHello.starts_with(Hello)) throw std::runtime_error("Hello format");
                const auto remote = std::span(reinterpret_cast<const unsigned char*>(remoteHello.data() + 37), 72);
                auto secret = peer.exchange->Agree(remote); Wipe wipeSecret{ secret };
                const std::string transcript = m_host ? std::string(remoteHello) + peer.hello : peer.hello + std::string(remoteHello);
                auto salt = NetworkHmac(m_roomKey, std::span(reinterpret_cast<const unsigned char*>(transcript.data()), transcript.size()));
                Wipe wipeSalt{ salt };
                auto client = NetworkHkdf(salt, secret, "LamaPon.Direct.1.client-to-host"); Wipe wipeClient{ client };
                auto host = NetworkHkdf(salt, secret, "LamaPon.Direct.1.host-to-client"); Wipe wipeHost{ host };
                peer.send.Initialize(m_host ? host : client); peer.receive.Initialize(m_host ? client : host);
                peer.exchange.reset();
            }
            void Receive(const TransportEvent& event, DirectPeer& peer, std::vector<TransportEvent>& result)
            {
                if (peer.stage == 2)
                {
                    result.push_back({ TransportEventKind::Message, event.peer, peer.receive.Open(event.data) });
                    return;
                }
                if (m_host && peer.stage == 0)
                {
                    Derive(peer, event.data);
                    if (!m_inner->Send(event.peer, peer.hello + peer.send.Seal(HostProof))) throw std::runtime_error("Queue");
                    peer.stage = 1;
                }
                else if (!m_host && peer.stage == 0)
                {
                    if (event.data.size() <= HelloSize) throw std::runtime_error("Host hello");
                    Derive(peer, std::string_view(event.data).substr(0, HelloSize));
                    if (peer.receive.Open(std::string_view(event.data).substr(HelloSize)) != HostProof) throw std::runtime_error("Host proof");
                    if (!m_inner->Send(event.peer, peer.send.Seal(ClientProof))) throw std::runtime_error("Queue");
                    peer.stage = 2; result.push_back({ TransportEventKind::Connected, event.peer, {} });
                }
                else if (m_host && peer.stage == 1)
                {
                    if (peer.receive.Open(event.data) != ClientProof) throw std::runtime_error("Client proof");
                    peer.stage = 2; result.push_back({ TransportEventKind::Connected, event.peer, {} });
                }
                else throw std::runtime_error("Handshake state");
            }
            void AuthenticationError(std::vector<TransportEvent>& result)
            {
                m_error = "Direct接続の認証に失敗しました。接続情報の秘密部分・期限・通信先を確認してください。";
                result.push_back({ TransportEventKind::Error, 0, m_error });
            }
            void Reject(const TransportPeer peer, std::vector<TransportEvent>& result)
            {
                const auto found = m_peers.find(peer);
                if (found != m_peers.end() && found->second->stage == 2)
                    result.push_back({ TransportEventKind::Disconnected, peer, {} });
                m_inner->Disconnect(peer); m_peers.erase(peer);
                if (!m_host) AuthenticationError(result);
            }
            bool m_host{};
            NetworkKey m_roomKey{};
            std::unique_ptr<INetworkTransport> m_inner;
            NetworkPortMapping m_mapping;
            std::map<TransportPeer, std::unique_ptr<DirectPeer>> m_peers;
            std::string m_endpoint, m_error;
        };
    }
    std::unique_ptr<INetworkTransport> CreateDirectTransport() { return std::make_unique<DirectTransport>(); }
}
