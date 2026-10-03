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
        // 接続入力と部屋検索を独立した状態として作る。
        NetworkConnectionPanel();
        // 部屋検索と接続入力の状態を解放する。
        ~NetworkConnectionPanel();
        // 再生中の接続・部屋検索・通信状態を表示する(session: 通信処理の借用・不在はnull, bridge: Scene接続処理の借用・省略可, playing: エディターが再生中か, configuration: 実行中の通信条件)。
        void Draw(NetworkSession* session, NetworkSceneBridge* bridge, bool playing,
            const NetworkConfiguration& configuration);
        // 実行中の部屋検索を止める。
        void StopSearch();
    private:
        struct Implementation;
        // 検索処理と接続入力の所有先
        std::unique_ptr<Implementation> m_impl;
    };
}
