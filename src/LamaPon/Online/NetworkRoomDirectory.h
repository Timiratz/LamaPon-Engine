#pragma once

#include "LamaPon/Online/NetworkSession.h"

namespace LamaPon
{
    struct NetworkRoom final
    {
        // 部屋の表示名
        std::string name;
        // ゲーム識別子
        std::string gameId;
        // ゲームの互換バージョン
        std::string gameVersion;
        // 接続先シーンの識別子
        std::string sceneId;
        // 通信バックエンド
        NetworkBackend backend{ NetworkBackend::Lan };
        // 現在の参加者数
        std::uint32_t players{};
        // 参加者数の上限
        std::uint32_t capacity{};
        // 部屋の接続情報
        std::string connection;
    };
    // ゲーム側の実装が寿命と認証を管理し、非同期通信の結果をUpdateで反映します。
    class INetworkRoomDirectory
    {
    public:
        // 部屋一覧の実装を破棄する。
        virtual ~INetworkRoomDirectory() = default;
        // 部屋検索を開始する(game: ゲーム識別用の通信設定)。
        virtual bool Search(const NetworkConfiguration& game) = 0;
        // 部屋の公開を開始する(room: 公開する部屋)。
        virtual bool Publish(const NetworkRoom& room) = 0;
        // 部屋の公開を取り下げる。
        virtual void Withdraw() = 0;
        // 通信結果を反映する(seconds: 経過秒数)。
        virtual void Update(float seconds) = 0;
        // 検索結果を借用し、有効期間は実装側の契約に従う。
        virtual const std::vector<NetworkRoom>& Rooms() const = 0;
        // 直近のエラーを返す。
        virtual std::string LastError() const = 0;
    };
}
