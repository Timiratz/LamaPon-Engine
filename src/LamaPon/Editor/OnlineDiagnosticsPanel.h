#pragma once

#include "LamaPon/Editor/NetworkConnectionPanel.h"
#include <array>
#include <string>

namespace LamaPon
{
    class OnlineServices;
    struct OnlineProjectSettings;

    // 診断用入力と検索の寿命を所有し、保存用のProject Settings草稿に依存しません。
    class OnlineDiagnosticsPanel final
    {
    public:
        void Draw(NetworkSession* session, NetworkSceneBridge* bridge, bool playing,
            OnlineServices& services, const OnlineProjectSettings& settings);
        void StopSearch() { m_network.StopSearch(); }
    private:
        NetworkConnectionPanel m_network;
        std::array<char, 129> m_details{ "エディターで動作確認" };
        std::array<char, 129> m_state{ "開発中" };
        std::string m_message;
    };
}
