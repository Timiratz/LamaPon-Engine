#pragma once

#include "LamaPon/Online/NetworkSession.h"
#include <memory>

namespace LamaPon::Detail
{
    class NetworkRoomAdvertiser final
    {
    public:
        // 部屋広告の状態を作る。
        NetworkRoomAdvertiser();
        // 広告ソケットを閉じて状態を破棄する。
        ~NetworkRoomAdvertiser();
        // LAN広告の待受を始める(configuration: 検証済み通信設定)。
        bool Start(const NetworkConfiguration& configuration);
        // ホストの部屋情報を要求元へ応答する(session: 広告するセッション)。
        // 1回に最大16要求を読み、送信失敗も含め1秒間の応答試行を64件に制限します。
        void Update(const NetworkSession& session);
        // 広告ソケットを閉じる。
        void Stop() noexcept;
    private:
        struct Implementation;
        // 広告状態の所有先
        std::unique_ptr<Implementation> m_impl;
    };
}
