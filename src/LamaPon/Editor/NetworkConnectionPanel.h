#pragma once

#include <memory>

namespace LamaPon
{
    class NetworkSession;
    class NetworkSceneBridge;
    struct NetworkConfiguration;
    // 接続用の入力・部屋検索はこのパネルが所有し、EditorLayerの状態を保持しません。
    class NetworkConnectionPanel final
    {
    public:
        NetworkConnectionPanel();
        ~NetworkConnectionPanel();
        void Draw(NetworkSession* session, NetworkSceneBridge* bridge, bool playing,
            const NetworkConfiguration& configuration);
        void StopSearch();
    private:
        struct Implementation;
        std::unique_ptr<Implementation> m_impl;
    };
}
