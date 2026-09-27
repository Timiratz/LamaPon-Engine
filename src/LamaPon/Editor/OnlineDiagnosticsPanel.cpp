#include "LamaPon/Editor/OnlineDiagnosticsPanel.h"
#include "LamaPon/Online/NetworkSession.h"
#include <imgui.h>

namespace LamaPon
{
    void OnlineDiagnosticsPanel::Draw(NetworkSession* session, NetworkSceneBridge* bridge, const bool playing)
    {
        if (!session)
        {
            StopSearch();
            ImGui::TextUnformatted("通信セッションがありません。");
            return;
        }
        const auto& configuration = session->Configuration();
        ImGui::TextWrapped("ゲーム: %s / バージョン: %s / シーン: %s",
            configuration.gameId.c_str(), configuration.gameVersion.c_str(), configuration.sceneId.c_str());
        m_network.Draw(session, bridge, playing, configuration);
    }
}
