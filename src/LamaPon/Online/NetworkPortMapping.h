#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace LamaPon::Detail
{
    struct NetworkPortEntry final
    {
        // 転送先の内部IPv4
        std::string client;
        // 転送先ポート番号
        std::uint16_t port{};
        // 所有者識別用の説明
        std::string description;
        // 転送の有効秒数
        std::uint32_t leaseSeconds{};
        // 転送設定の有効状態
        bool enabled{ true };
    };
    class INetworkGateway
    {
    public:
        // ルーター操作の実装を破棄する。
        virtual ~INetworkGateway() = default;
        // TCP転送を調べ、通信失敗は例外・未登録はnulloptにする(port: 外部ポート番号)。
        virtual std::optional<NetworkPortEntry> Inspect(std::uint16_t port) = 0;
        // TCPポート転送を登録する(externalPort: 外部ポート番号, entry: 転送先と期限)。
        virtual bool Add(std::uint16_t externalPort, const NetworkPortEntry& entry) = 0;
        // TCPポート転送を削除する(externalPort: 外部ポート番号)。
        virtual void Remove(std::uint16_t externalPort) = 0;
        // ルーターの外部IPv4アドレスを返す。
        virtual std::string ExternalAddress() = 0;
    };
    // gatewayはこのleaseと更新・解放処理より長く生存する必要があります。
    class NetworkPortLease final
    {
    public:
        // 120秒のTCP転送要求を作る(gateway: 借用するルーター操作, client: 転送先IPv4, port: 内外共通ポート, description: 所有者識別用の説明)。
        NetworkPortLease(INetworkGateway& gateway, std::string client, std::uint16_t port,
            std::string description);
        // 自身の転送を解放する。
        ~NetworkPortLease();
        // 未登録のポートだけを確保し、短期転送の所有と有効性を確認する。
        bool Acquire();
        // 自身が所有する短期転送の期限を更新する。
        bool Renew();
        // 所有を再確認できた転送だけを削除し、通信例外を抑える。
        void Release() noexcept;
    private:
        // 転送先・ポート・説明で所有者を照合する(entry: 現在の転送設定)。
        bool Owns(const NetworkPortEntry& entry) const;
        // ルーター操作の借用先
        INetworkGateway& m_gateway;
        // 要求する短期転送の内容
        NetworkPortEntry m_entry;
        // 転送を確保済みか
        bool m_owned{};
    };
    class NetworkPortMapping final
    {
    public:
        // 非同期ポート転送の共有状態を作る。
        NetworkPortMapping();
        // 処理の停止を要求する。
        ~NetworkPortMapping();
        // バックグラウンドでIPv4ルーターの短期転送を探して設定する(listenAddress: TCP待受アドレス)。
        void Start(const std::string& listenAddress);
        // 応答待ちをブロックせず停止を要求し、ワーカーに転送を解放させる。
        void Stop() noexcept;
        // 確認できた公開接続先を返す。
        std::string Endpoint() const;
        // 非同期処理の状態を返す。
        std::string Status() const;
    private:
        struct Implementation;
        // ワーカーと共有する状態
        std::shared_ptr<Implementation> m_impl;
    };
    // プライベート・共有・特殊用途を除いたIPv4かを判定する(address: 検査するアドレス)。
    bool IsPublicNetworkIpv4(const std::string& address);
}
