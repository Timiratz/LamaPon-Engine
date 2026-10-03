#include "LamaPon/Editor/ServiceDiagnosticsPanel.h"
#include "LamaPon/Core/ProjectSettings.h"
#include "LamaPon/Online/OnlineServices.h"
#include <imgui.h>
#include <utility>

namespace LamaPon
{
    void ServiceDiagnosticsPanel::Draw(OnlineServices& services, const OnlineProjectSettings& settings)
    {
        ImGui::SeparatorText("Discord Rich Presence");
        ImGui::TextWrapped("保存済みのプロジェクト共通設定を使ってテスト表示を送ります。診断用のDetailsとStateは保存しません。");
        // 保存済みのPresence共通設定
        const auto& common = settings.discordPresence;
        ImGui::Text("Application ID: %s", common.applicationId.c_str());
        ImGui::InputText("Details", m_details.data(), m_details.size());
        ImGui::InputText("State", m_state.data(), m_state.size());
        ImGui::BeginDisabled(common.applicationId.empty());
        if (ImGui::Button("テスト表示を送信"))
        {
            // テスト表示用のPresence構成
            DiscordPresenceConfiguration configuration;
            configuration.enabled = true;
            configuration.applicationId = common.applicationId;
            configuration.defaultLargeImageKey = common.defaultLargeImageKey;
            configuration.defaultLargeImageText = common.defaultLargeImageText;
            services.ConfigureDiscordPresence(std::move(configuration));
            // 操作するPresence処理の借用
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
        // 画面へ表示するPresence状態名
        const auto name = DiscordPresenceStateName(services.Presence().State());
        ImGui::Text("状態: %.*s", static_cast<int>(name.size()), name.data());
        if (!m_message.empty()) ImGui::TextWrapped("%s", m_message.c_str());
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("Application IDはプロジェクト設定で保存します。DiscordまたはPresenceアダプターがない場合はUnavailableになります。");
        ImGui::PopStyleColor();
    }
}
