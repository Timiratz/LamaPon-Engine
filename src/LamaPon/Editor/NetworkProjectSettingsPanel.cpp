#include "LamaPon/Editor/EditorLayer.h"

#include "LamaPon/Online/NetworkSceneBridge.h"
#include "LamaPon/Online/NetworkSession.h"

#include <imgui.h>

#include <array>
#include <algorithm>

namespace LamaPon
{
    namespace
    {
        void EditNetworkText(const char* label, std::string& value)
        {
            std::array<char, 513> buffer{};
            strncpy_s(buffer.data(), buffer.size(), value.c_str(), _TRUNCATE);
            if (ImGui::InputText(label, buffer.data(), buffer.size())) value = buffer.data();
        }
    }

    void EditorLayer::DrawProjectSettingsNetworkSection()
    {
        ImGui::SeparatorText("プレイヤー同士の通信（P2P）");
        auto* session = ActiveNetworkSession();
        const bool active = session && session->State() != NetworkState::Stopped
            && session->State() != NetworkState::Error;
        ImGui::BeginDisabled(active);
        int backend = m_projectNetworkDraft.backend == NetworkBackend::Direct ? 0
            : (m_projectNetworkDraft.backend == NetworkBackend::Lan ? 1 : 2);
        if (ImGui::Combo("接続方式", &backend, "直接接続（Epic不要・暗号化）\0LAN / 同じPC（従来方式）\0インターネット（EOS）\0"))
        {
            m_projectNetworkDraft.backend = backend == 0 ? NetworkBackend::Direct
                : (backend == 1 ? NetworkBackend::Lan : NetworkBackend::EpicOnlineServices);
            m_networkConnectionPanel.StopSearch();
        }
        EditNetworkText("ゲームID##P2P", m_projectNetworkDraft.gameId);
        EditNetworkText("通信バージョン", m_projectNetworkDraft.gameVersion);
        EditNetworkText("シーンID", m_projectNetworkDraft.sceneId);
        int players = static_cast<int>(m_projectNetworkDraft.maxPlayers);
        if (ImGui::SliderInt("最大人数（ホストを含む）", &players, 2, 4))
            m_projectNetworkDraft.maxPlayers = static_cast<std::uint32_t>(players);
        int rate = static_cast<int>(m_projectNetworkDraft.tickRate);
        if (ImGui::SliderInt("状態送信の頻度（Hz）", &rate, 1, 60))
            m_projectNetworkDraft.tickRate = static_cast<std::uint32_t>(rate);
        int syncMode = m_projectNetworkDraft.syncMode == NetworkSyncMode::OnChange ? 1 : 0;
        if (ImGui::Combo("オブジェクトの同期", &syncMode, "定期送信（アクションなど）\0変更時だけ（ターン制など）\0"))
            m_projectNetworkDraft.syncMode = syncMode == 0 ? NetworkSyncMode::Continuous : NetworkSyncMode::OnChange;
        ImGui::TextWrapped("操作要求・部屋全体の状態・ゲームイベントも使えます。ジャンルに合わせてゲーム側で組み合わせます。");
        ImGui::SliderFloat("切断待ち時間（秒）", &m_projectNetworkDraft.timeoutSeconds, 5, 120);
        if (m_projectNetworkDraft.backend != NetworkBackend::EpicOnlineServices)
        {
            int port = m_projectNetworkDraft.port;
            if (ImGui::InputInt("待受ポート", &port))
                m_projectNetworkDraft.port = static_cast<std::uint16_t>(std::clamp(port, 0, 65535));
            ImGui::TextWrapped("ポート0は空いているポートを自動選択します。IPv6の接続先は [アドレス]:port の形式です。");
            EditNetworkText("部屋の名前", m_projectNetworkDraft.roomName);
            ImGui::Checkbox("LAN内の部屋検索へ公開する", &m_projectNetworkDraft.advertiseLan);
            if (m_projectNetworkDraft.advertiseLan)
            {
                ImGui::TextWrapped("LAN内の参加者が一覧から接続できる公開部屋になります。非公開で遊ぶ場合はOFFにしてください。");
                int discoveryPort = m_projectNetworkDraft.discoveryPort;
                if (ImGui::InputInt("LAN検索ポート", &discoveryPort))
                    m_projectNetworkDraft.discoveryPort = static_cast<std::uint16_t>(std::clamp(discoveryPort, 1, 65535));
            }
            if (m_projectNetworkDraft.backend == NetworkBackend::Direct)
            {
                ImGui::Checkbox("ルーターの自動ポート設定を利用する（UPnP）", &m_projectNetworkDraft.automaticPortMapping);
                ImGui::TextWrapped("ONの場合だけ、対応IPv4ルーターへ120秒のTCPポート転送を要求し、接続中は更新、終了時は解除します。永久的な設定は残しません。Windowsファイアウォールは自動変更しません。");
            }
            else ImGui::TextWrapped("従来LAN方式には暗号化・参加認証がありません。信頼できるLAN内で使ってください。");
        }
        else
        {
            EditNetworkText("EOS Product ID", m_projectNetworkDraft.eosProductId);
            EditNetworkText("EOS Sandbox ID", m_projectNetworkDraft.eosSandboxId);
            EditNetworkText("EOS Deployment ID", m_projectNetworkDraft.eosDeploymentId);
            EditNetworkText("EOS Client ID", m_projectNetworkDraft.eosClientId);
            EditNetworkText("EOS資格情報の環境変数名", m_projectNetworkDraft.eosClientSecretEnvironment);
            ImGui::TextWrapped("ホストの部屋IDを共有して参加します。接続にはEOSの製品設定とSDK対応ビルドが必要です。");
            if (!HasEpicNetworkBackend())
                ImGui::TextColored(ImVec4{ 1, 0.65f, 0.25f, 1 }, "このビルドにはEOS SDKがありません。");
        }
        if (ImGui::TreeNode("同期Prefabの登録"))
        {
            ImGui::TextWrapped("両方のゲームに同じキーとPrefabを登録します。パスはassetsからの相対パスです。");
            for (std::size_t index = 0; index < m_projectNetworkDraft.prefabs.size(); ++index)
            {
                ImGui::PushID(static_cast<int>(index));
                auto& prefab = m_projectNetworkDraft.prefabs[index];
                EditNetworkText("キー", prefab.key);
                EditNetworkText("Prefabパス", prefab.assetPath);
                const bool remove = ImGui::Button("登録を削除");
                ImGui::Separator();
                ImGui::PopID();
                if (remove)
                {
                    m_projectNetworkDraft.prefabs.erase(m_projectNetworkDraft.prefabs.begin()
                        + static_cast<std::ptrdiff_t>(index));
                    break;
                }
            }
            if (m_projectNetworkDraft.prefabs.size() < 64 && ImGui::Button("Prefabを登録"))
                m_projectNetworkDraft.prefabs.push_back({ "player", "prefabs/player.prefab.json" });
            ImGui::TreePop();
        }
        ImGui::EndDisabled();

        m_networkConnectionPanel.Draw(session, ActiveNetworkSceneBridge(), m_playing, m_projectNetworkDraft);

    }
}
