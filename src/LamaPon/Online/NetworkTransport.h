#pragma once

#include "LamaPon/Online/NetworkSession.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace LamaPon::Detail
{
    // パケットの上限バイト数
    inline constexpr std::size_t NetworkPacketMaxBytes = 1100;
    // 同期オブジェクト数の上限
    inline constexpr std::size_t NetworkObjectLimit = 128;
    // 保留イベント数の上限
    inline constexpr std::size_t NetworkEventLimit = 512;

    using TransportPeer = std::uint64_t;
    enum class TransportEventKind { Ready, Connected, Disconnected, Message, Error };
    struct TransportEvent final
    {
        // 通信イベントの種別
        TransportEventKind kind{};
        // 対象の接続ID
        TransportPeer peer{};
        // 受信内容またはエラー
        std::string data;
    };

    // 各バックエンドは接続ごとの信頼性と順序を保証し、送受信キューとPollの作業量を制限します。
    class INetworkTransport
    {
    public:
        // バックエンドを破棄する。
        virtual ~INetworkTransport() = default;
        // 通信を開始する(configuration: 通信設定, host: ホスト側か, address: 接続先, name: 参加者名)。
        virtual bool Start(const NetworkConfiguration& configuration,
            bool host, const std::string& address, const std::string& name) = 0;
        // 通信を停止する。
        virtual void Stop() noexcept = 0;
        // 通信を進めてイベントを取り出す(elapsedSeconds: 経過秒数)。
        virtual std::vector<TransportEvent> Poll(float elapsedSeconds) = 0;
        // 送信を要求する(peer: 宛先, packet: 送信パケット)。
        virtual bool Send(TransportPeer peer, const std::string& packet) = 0;
        // 接続を切断する(peer: 切断対象)。
        virtual void Disconnect(TransportPeer peer) = 0;
        // 接続先として公開するアドレスを返す。
        virtual std::string Address() const = 0;
        // 通信エラーを返す。
        virtual std::string Error() const = 0;
        // 接続用の秘密鍵文字列を返す。
        virtual std::string AccessKey() const { return {}; }
        // 接続コードを返す。
        virtual std::string ConnectionCode(const std::string&) const { return Address(); }
        // バックエンドの状態を返す。
        virtual std::string Status() const { return {}; }
        // ローカルの待受アドレスを返す。
        virtual std::string LocalAddress() const { return Address(); }
    };

    // LANバックエンドを所有権付きで作る。
    std::unique_ptr<INetworkTransport> CreateLanTransport();
    // EOSバックエンドを作り、EOS無効のビルドでは空を返す。
    std::unique_ptr<INetworkTransport> CreateEpicTransport();
    // 直接接続バックエンドを所有権付きで作る。
    std::unique_ptr<INetworkTransport> CreateDirectTransport();
}
