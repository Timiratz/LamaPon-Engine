#pragma once

#include "LamaPon/Editor/NetworkConnectionPanel.h"

namespace LamaPon
{
    // ゲーム通信の診断と部屋検索だけを所有します。
    class OnlineDiagnosticsPanel final
    {
    public:
        // 通信条件を表示して接続操作へ渡しsession不在なら検索を止める(session: 通信処理の借用・不在はnull, bridge: Scene接続処理の借用・省略可, playing: エディターが再生中か)。
        void Draw(NetworkSession* session, NetworkSceneBridge* bridge, bool playing);
        // 接続パネルの部屋検索を止める。
        void StopSearch() { m_network.StopSearch(); }
    private:
        // 接続入力と部屋検索の所有先
        NetworkConnectionPanel m_network;
    };
}
