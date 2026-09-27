#include "LamaPon/Editor/OnlineDiagnosticsPanel.h"
#include "LamaPon/Core/ProjectSettings.h"
#include "LamaPon/Online/OnlineServices.h"
#include "LamaPon/Online/NetworkSession.h"
#include <imgui.h>

namespace LamaPon
{
    void OnlineDiagnosticsPanel::Draw(NetworkSession* session, NetworkSceneBridge* bridge,
        const bool playing, OnlineServices& services, const OnlineProjectSettings& settings)
    {
        if (!ImGui::BeginTabBar("OnlineDiagnostics")) return;
        if (ImGui::BeginTabItem("P2P通信"))
        {
            if (session)
            {
                const auto& configuration = session->Configuration();
                ImGui::TextWrapped("ゲーム: %s / バージョン: %s / シーン: %s",
                    configuration.gameId.c_str(), configuration.gameVersion.c_str(), configuration.sceneId.c_str());
                m_network.Draw(session, bridge, playing, configuration);
            }
            else ImGui::TextUnformatted("通信セッションがありません。");
            ImGui::EndTabItem();
        }
        else StopSearch();
        if (ImGui::BeginTabItem("Discord Rich Presence"))
        {
            ImGui::TextWrapped("保存済みのプロジェクト共通設定を使ってテスト表示を送ります。診断用のDetailsとStateは保存しません。");
            const auto& common = settings.discordPresence;
            ImGui::Text("Application ID: %s", common.applicationId.c_str());
            ImGui::InputText("Details", m_details.data(), m_details.size());
            ImGui::InputText("State", m_state.data(), m_state.size());
            ImGui::BeginDisabled(common.applicationId.empty());
            if (ImGui::Button("テスト表示を送信"))
            {
                DiscordPresenceConfiguration configuration;
                configuration.enabled = true;
                configuration.applicationId = common.applicationId;
                configuration.defaultLargeImageKey = common.defaultLargeImageKey;
                configuration.defaultLargeImageText = common.defaultLargeImageText;
                services.ConfigureDiscordPresence(std::move(configuration));
                auto& presence = services.Presence();
                m_message = presence.SetActivity(m_details.data(), m_state.data())
                    ? "テスト表示を送信しました。" : presence.LastError();
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("表示を消す"))
            {
                services.Presence().ClearActivity();
                m_message = "表示を消しました。";
            }
            const auto name = DiscordPresenceStateName(services.Presence().State());
            ImGui::Text("状態: %.*s", static_cast<int>(name.size()), name.data());
            if (!m_message.empty()) ImGui::TextWrapped("%s", m_message.c_str());
            ImGui::TextDisabled("Application IDはプロジェクト設定で保存します。DiscordまたはPresenceアダプターがない場合はUnavailableになります。");
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}
