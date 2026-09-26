#pragma once

#include "LamaPon/Online/NetworkSession.h"

namespace LamaPon
{
    struct NetworkRoom final
    {
        std::string name;
        std::string gameId;
        std::string gameVersion;
        std::string sceneId;
        NetworkBackend backend{ NetworkBackend::Lan };
        std::uint32_t players{};
        std::uint32_t capacity{};
        // 接続情報には秘密部分を含む場合があります。ログや設定ファイルへ保存しません。
        std::string connection;
    };
    // 公開一覧やマッチングの実装はゲームが所有します。SDKやサービスを固定しません。
    // 通信を非同期で開始し、Updateで結果を反映してください。寿命と認証も実装側が管理します。
    class INetworkRoomDirectory
    {
    public:
        virtual ~INetworkRoomDirectory() = default;
        virtual bool Search(const NetworkConfiguration& game) = 0;
        virtual bool Publish(const NetworkRoom& room) = 0;
        virtual void Withdraw() = 0;
        virtual void Update(float seconds) = 0;
        virtual const std::vector<NetworkRoom>& Rooms() const = 0;
        virtual std::string LastError() const = 0;
    };
}
