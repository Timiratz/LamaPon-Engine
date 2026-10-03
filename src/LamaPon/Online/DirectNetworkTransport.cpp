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
        // Direct接続コードの接頭辞
        constexpr std::string_view Prefix = "LPD1|";
        // 鍵交換開始の形式識別子
        constexpr std::string_view Hello = "LPDH1";
        // 識別子・nonce・公開鍵の長さ
        constexpr std::size_t HelloSize = 5 + 32 + 72;
        // ホスト認証の確認文字列
        constexpr std::string_view HostProof = "LamaPon.Direct.Host.1";
        // 参加側認証の確認文字列
        constexpr std::string_view ClientProof = "LamaPon.Direct.Client.1";
        struct Wipe final
        {
            // 消去する秘密鍵の借用先
            NetworkKey& key;
            // 借用した秘密鍵をスコープ終了時に消去する。
            ~Wipe() { SecureZeroMemory(key.data(), key.size()); }
        };
        struct DirectPeer final
        {
            // 接続専用のECDH鍵交換
            std::unique_ptr<NetworkKeyExchange> exchange{ std::make_unique<NetworkKeyExchange>() };
            // 送信・受信用の暗号器
            NetworkCipher send, receive;
            // 鍵交換に含める32バイト乱数
            NetworkKey nonce{ RandomNetworkKey() };
            // 識別子と乱数と公開鍵
            std::string hello;
            // 認証段階(0初期・1待機・2完了)
            int stage{};
            // 認証開始からの経過秒数
            float age{};
            // 接続専用の乱数と公開鍵で鍵交換要求を作る。
            DirectPeer()
            {
                hello = Hello;
                hello.append(reinterpret_cast<const char*>(nonce.data()), nonce.size());
                // 72バイトのCNG公開鍵
                const auto key = exchange->PublicKey();
                hello.append(reinterpret_cast<const char*>(key.data()), key.size());
            }
            // 鍵交換用の乱数を消去する。
            ~DirectPeer() { SecureZeroMemory(nonce.data(), nonce.size()); }
        };
        class DirectTransport final : public INetworkTransport
        {
        public:
            // TCPと暗号状態を停止する。
            ~DirectTransport() override { Stop(); }
            // 共有アクセスキーを用意してTCPを起動する(configuration: 通信設定, host: ホスト側か, address: 待受先または接続コード, name: 参加者名)。
            bool Start(const NetworkConfiguration& configuration, const bool host,
                const std::string& address, const std::string& name) override
            {
                Stop(); m_error.clear(); m_host = host;
                // 解析したTCP接続先
                std::string endpoint = address;
                if (host)
                {
                    try { m_roomKey = RandomNetworkKey(); }
                    catch (...) { m_error = "通信の秘密情報を作成できません。"; return false; }
                }
                else
                {
                    if (!address.starts_with(Prefix)) { m_error = "Direct接続には接続情報、または接続先とアクセスキーが必要です。"; return false; }
                    // 接続先と秘密鍵の区切り位置
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
            // ポート転送・TCP・暗号状態を止め、アクセスキーを消去する。
            void Stop() noexcept override
            {
                m_mapping.Stop();
                if (m_inner) m_inner->Stop();
                m_inner.reset(); m_peers.clear(); m_endpoint.clear();
                SecureZeroMemory(m_roomKey.data(), m_roomKey.size());
            }
            // 認証済みイベントを取り出し、5秒を超えた未認証接続を切る(seconds: 経過秒数)。
            std::vector<TransportEvent> Poll(const float seconds) override
            {
                // 認証後に返す通信イベント
                std::vector<TransportEvent> result;
                if (!m_inner) return result;
                // 接続IDと認証処理の状態
                for (auto& [id, peer] : m_peers)
                { static_cast<void>(id); if (peer->stage != 2) peer->age += seconds; }
                // TCPバックエンドのイベント
                for (const auto& event : m_inner->Poll(seconds))
                {
                    if (event.kind == TransportEventKind::Connected)
                    {
                        try
                        {
                            // 接続専用の鍵交換状態
                            auto peer = std::make_unique<DirectPeer>();
                            if (!m_host && !m_inner->Send(event.peer, peer->hello)) throw std::runtime_error("Handshake queue");
                            m_peers.emplace(event.peer, std::move(peer));
                        }
                        catch (...) { Reject(event.peer, result); }
                    }
                    else if (event.kind == TransportEventKind::Message)
                    {
                        // 接続IDに対応する暗号状態
                        const auto found = m_peers.find(event.peer); if (found == m_peers.end()) continue;
                        try { Receive(event, *found->second, result); }
                        catch (...) { Reject(event.peer, result); }
                    }
                    else if (event.kind == TransportEventKind::Disconnected)
                    {
                        // 接続IDに対応する暗号状態
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
                // 認証期限を超えた接続ID
                std::vector<TransportPeer> expired;
                // 接続IDと認証処理の状態
                for (const auto& [id, peer] : m_peers) if (peer->stage != 2 && peer->age > 5) expired.push_back(id);
                // 切断する接続ID
                for (const auto id : expired) Reject(id, result);
                return result;
            }
            // 認証済み相手への暗号文を送信キューに積む(id: 宛先接続ID, packet: 平文パケット)。
            bool Send(const TransportPeer id, const std::string& packet) override
            {
                // 接続IDに対応する暗号状態
                const auto found = m_peers.find(id);
                if (!m_inner || found == m_peers.end() || found->second->stage != 2
                    || packet.empty() || packet.size() > NetworkPacketMaxBytes) return false;
                try { return m_inner->Send(id, found->second->send.Seal(packet)); }
                catch (...) { return false; }
            }
            // TCP切断を要求する(id: 切断する接続ID)。
            void Disconnect(const TransportPeer id) override { if (m_inner) m_inner->Disconnect(id); }
            // 直近の接続エラーを返す。
            std::string Error() const override { return m_error; }
            // 起動中のホストだけが共有アクセスキーを返す。
            std::string AccessKey() const override { return m_inner && m_host ? Hex(m_roomKey) : std::string{}; }
            // 選択した公開接続先を含むDirect接続コードを返す。
            std::string Address() const override { return ConnectionCode({}); }
            // TCPの実際のローカル待受先を返す。
            std::string LocalAddress() const override { return m_endpoint; }
            // 公開宛先と共有鍵から接続コードを作る(requested: 指定宛先・空なら自動選択)。
            std::string ConnectionCode(const std::string& requested) const override
            {
                if (!m_inner) return {};
                // TCPのローカル待受先
                NetworkEndpoint local;
                if (!NetworkEndpoint::Parse(m_endpoint, 0, local)) return {};
                // 公開接続先の文字列
                std::string address = requested.empty() ? m_mapping.Endpoint() : requested;
                if (address.empty()) address = local.Wildcard()
                    ? (local.Family() == AF_INET ? "127.0.0.1:" : "[::1]:") + std::to_string(local.Port()) : local.Text();
                // 解析したTCP接続先
                NetworkEndpoint endpoint;
                if (!NetworkEndpoint::Parse(address, local.Port(), endpoint) || !endpoint.Unicast() || endpoint.Port() == 0) return {};
                return std::string(Prefix) + endpoint.Text() + "|" + Hex(m_roomKey);
            }
            // 待受先と暗号化・ポート転送の状態を返す。
            std::string Status() const override
            {
                // 自動ポート転送の状態
                const auto mapping = m_mapping.Status();
                return "Direct: 共有アクセスキーで認証、ECDH / AES-256-GCMで暗号化。待受 / 接続先: " + m_endpoint
                    + (mapping.empty() ? "。外部回線では公開接続先とポート転送、または到達可能なIPv6が必要です。" : "。" + mapping);
            }
        private:
            // 認証した鍵交換内容から方向別の鍵を導出する(peer: 接続専用の暗号状態, remoteHello: 相手の鍵交換要求)。
            void Derive(DirectPeer& peer, const std::string_view remoteHello)
            {
                if (remoteHello.size() != HelloSize || !remoteHello.starts_with(Hello)) throw std::runtime_error("Hello format");
                // 相手の72バイト公開鍵
                const auto remote = std::span(reinterpret_cast<const unsigned char*>(remoteHello.data() + 37), 72);
                // ECDHで得た共有秘密
                auto secret = peer.exchange->Agree(remote);
                // 共有秘密を必ず消去する保護
                Wipe wipeSecret{ secret };
                // 参加側・ホスト順の鍵交換内容
                const std::string transcript = m_host ? std::string(remoteHello) + peer.hello : peer.hello + std::string(remoteHello);
                // 共有アクセスキーで認証した素材
                auto salt = NetworkHmac(m_roomKey, std::span(reinterpret_cast<const unsigned char*>(transcript.data()), transcript.size()));
                // 導出素材を必ず消去する保護
                Wipe wipeSalt{ salt };
                // 参加側からホストへの暗号鍵
                auto client = NetworkHkdf(salt, secret, "LamaPon.Direct.1.client-to-host");
                // 参加側の鍵を消去する保護
                Wipe wipeClient{ client };
                // ホストから参加側への暗号鍵
                auto host = NetworkHkdf(salt, secret, "LamaPon.Direct.1.host-to-client");
                // ホスト側の鍵を消去する保護
                Wipe wipeHost{ host };
                peer.send.Initialize(m_host ? host : client); peer.receive.Initialize(m_host ? client : host);
                peer.exchange.reset();
            }
            // 鍵交換を進め、認証後はパケットを復号する(event: TCP受信イベント, peer: 接続専用の暗号状態, result: イベントの出力先)。
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
            // 認証失敗を状態とイベントに残す(result: エラーイベントの出力先)。
            void AuthenticationError(std::vector<TransportEvent>& result)
            {
                m_error = "Direct接続の認証に失敗しました。接続情報の秘密部分・期限・通信先を確認してください。";
                result.push_back({ TransportEventKind::Error, 0, m_error });
            }
            // 認証状態を破棄してTCPを切断する(peer: 切断する接続ID, result: イベントの出力先)。
            void Reject(const TransportPeer peer, std::vector<TransportEvent>& result)
            {
                // 接続IDに対応する暗号状態
                const auto found = m_peers.find(peer);
                if (found != m_peers.end() && found->second->stage == 2)
                    result.push_back({ TransportEventKind::Disconnected, peer, {} });
                m_inner->Disconnect(peer); m_peers.erase(peer);
                if (!m_host) AuthenticationError(result);
            }
            // ホスト側として動作するか
            bool m_host{};
            // 部屋の共有アクセスキー
            NetworkKey m_roomKey{};
            // TCPバックエンドの所有先
            std::unique_ptr<INetworkTransport> m_inner;
            // 非同期の短期ポート転送
            NetworkPortMapping m_mapping;
            // 接続ID別の認証と暗号状態
            std::map<TransportPeer, std::unique_ptr<DirectPeer>> m_peers;
            // TCP待受先と直近エラー
            std::string m_endpoint, m_error;
        };
    }
    // TCPを認証・暗号化するDirect通信実装を作る。
    std::unique_ptr<INetworkTransport> CreateDirectTransport() { return std::make_unique<DirectTransport>(); }
}
