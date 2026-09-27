#pragma once

#include "LamaPon/Editor/NetworkConnectionPanel.h"

namespace LamaPon
{
    // ゲーム通信の診断と部屋検索だけを所有します。
    class OnlineDiagnosticsPanel final
    {
    public:
        void Draw(NetworkSession* session, NetworkSceneBridge* bridge, bool playing);
        void StopSearch() { m_network.StopSearch(); }
    private:
        NetworkConnectionPanel m_network;
    };
}
