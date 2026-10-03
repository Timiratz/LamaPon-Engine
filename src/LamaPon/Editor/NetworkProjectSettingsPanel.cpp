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
        // 長さを制限した入力で通信設定の文字列を編集する(label: 入力欄の表示名, value: 編集する設定文字列)。
        void EditNetworkText(const char* label, std::string& value)
        {
            // 編集する通信設定のUTF8バッファ
            std::array<char, 513> buffer{};
            strncpy_s(buffer.data(), buffer.size(), value.c_str(), _TRUNCATE);
            if (ImGui::InputText(label, buffer.data(), buffer.size())) value = buffer.data();
        }
    }

    // 通信共通設定と従来のP2P設定・同期Prefab登録を編集する。
    void EditorLayer::DrawProjectSettingsNetworkSection()
    {
        ImGui::SeparatorText("通信サービス共通設定");
        ImGui::TextWrapped("ゲームごとのIDとサービス設定を保存します。Sceneごとの通信条件はNetwork Session Workflowの通信設定アセットと通信セッション管理Scriptで設定します。");
        EditNetworkText("P2PゲームID", m_projectNetworkDraft.gameId);
        EditNetworkText("通信バージョン", m_projectNetworkDraft.gameVersion);
        if (ImGui::TreeNode("Epic Online Services共通設定"))
        {
            EditNetworkText("EOS Product ID", m_projectNetworkDraft.eosProductId);
            EditNetworkText("EOS Sandbox ID", m_projectNetworkDraft.eosSandboxId);
            EditNetworkText("EOS Deployment ID", m_projectNetworkDraft.eosDeploymentId);
            EditNetworkText("EOS Client ID", m_projectNetworkDraft.eosClientId);
            EditNetworkText("EOS資格情報の環境変数名", m_projectNetworkDraft.eosClientSecretEnvironment);
            if (!HasEpicNetworkBackend())
                ImGui::TextColored(ImVec4{ 1, 0.65f, 0.25f, 1 }, "このビルドにはEOS SDKがありません。");
            ImGui::TreePop();
        }
        ImGui::TextDisabled("接続・参加・検索は「ウィンドウ > オンライン診断」で確認できます。");
        if (!ImGui::TreeNode("従来のP2P設定（通信管理ScriptがないScene向け）")) return;
        ImGui::TextWrapped("保存済みの設定を引き続き使えます。通信管理Scriptを置いたSceneでは参照するアセットの条件が優先されます。変更は保存後、次の再生・書き出しに反映します。");
        // 接続方式の選択番号
        int backend = m_projectNetworkDraft.backend == NetworkBackend::Direct ? 0
            : (m_projectNetworkDraft.backend == NetworkBackend::Lan ? 1 : 2);
        if (ImGui::Combo("接続方式", &backend, "直接接続（暗号化）\0LAN / 同じPC（従来方式）\0インターネット（EOS）\0"))
        {
            m_projectNetworkDraft.backend = backend == 0 ? NetworkBackend::Direct
                : (backend == 1 ? NetworkBackend::Lan : NetworkBackend::EpicOnlineServices);
            m_onlineDiagnosticsPanel.StopSearch();
        }
        EditNetworkText("シーンID", m_projectNetworkDraft.sceneId);
        // 最大参加人数の編集値
        int players = static_cast<int>(m_projectNetworkDraft.maxPlayers);
        if (ImGui::SliderInt("最大人数（ホストを含む）", &players, 2, 4))
            m_projectNetworkDraft.maxPlayers = static_cast<std::uint32_t>(players);
        // 状態送信の編集頻度・Hz
        int rate = static_cast<int>(m_projectNetworkDraft.tickRate);
        if (ImGui::SliderInt("状態送信の頻度（Hz）", &rate, 1, 60))
            m_projectNetworkDraft.tickRate = static_cast<std::uint32_t>(rate);
        // オブジェクト同期方式の選択番号
        int syncMode = m_projectNetworkDraft.syncMode == NetworkSyncMode::OnChange ? 1 : 0;
        if (ImGui::Combo("オブジェクトの同期", &syncMode, "定期送信（アクションなど）\0変更時だけ（ターン制など）\0"))
            m_projectNetworkDraft.syncMode = syncMode == 0 ? NetworkSyncMode::Continuous : NetworkSyncMode::OnChange;
        ImGui::TextWrapped("操作要求・部屋全体の状態・ゲームイベントも使えます。ジャンルに合わせてゲーム側で組み合わせます。");
        ImGui::SliderFloat("切断待ち時間（秒）", &m_projectNetworkDraft.timeoutSeconds, 5, 120);
        if (m_projectNetworkDraft.backend != NetworkBackend::EpicOnlineServices)
        {
            // 待受ポートの編集値
            int port = m_projectNetworkDraft.port;
            if (ImGui::InputInt("待受ポート", &port))
                m_projectNetworkDraft.port = static_cast<std::uint16_t>(std::clamp(port, 0, 65535));
            ImGui::TextWrapped("ポート0は空いているポートを自動選択します。IPv6の接続先は [アドレス]:port の形式です。");
            EditNetworkText("部屋の名前", m_projectNetworkDraft.roomName);
            ImGui::Checkbox("LAN内の部屋検索へ公開する", &m_projectNetworkDraft.advertiseLan);
            if (m_projectNetworkDraft.advertiseLan)
            {
                ImGui::TextWrapped("LAN内の参加者が一覧から接続できる公開部屋になります。非公開で遊ぶ場合はOFFにしてください。");
                // LAN検索ポートの編集値
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
        if (ImGui::TreeNode("同期Prefabの登録"))
        {
            ImGui::TextWrapped("両方のゲームに同じキーとPrefabを登録します。パスはassetsからの相対パスです。");
            // 編集する登録Prefabの番号
            for (std::size_t index = 0; index < m_projectNetworkDraft.prefabs.size(); ++index)
            {
                ImGui::PushID(static_cast<int>(index));
                // 編集中の同期Prefabの登録
                auto& prefab = m_projectNetworkDraft.prefabs[index];
                EditNetworkText("キー", prefab.key);
                EditNetworkText("Prefabパス", prefab.assetPath);
                // 現在のPrefab登録を削除するか
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
        ImGui::TreePop();
    }

    // オンライン診断を描画し非表示または閉じたら検索を止める(open: パネルの表示状態の参照)。
    void EditorLayer::DrawOnlineDiagnosticsPanel(bool& open)
    {
        ImGui::SetNextWindowSize(ImVec2{ 680.0f, 600.0f }, ImGuiCond_FirstUseEver);
        if (ImGui::Begin("オンライン診断", &open))
            m_onlineDiagnosticsPanel.Draw(ActiveNetworkSession(), ActiveNetworkSceneBridge(),
                m_playing);
        else m_onlineDiagnosticsPanel.StopSearch();
        ImGui::End();
        if (!open) m_onlineDiagnosticsPanel.StopSearch();
    }

    // Presenceのテスト操作を診断パネルへ渡す(open: パネルの表示状態の参照)。
    void EditorLayer::DrawServiceDiagnosticsPanel(bool& open)
    {
        ImGui::SetNextWindowSize(ImVec2{ 680.0f, 400.0f }, ImGuiCond_FirstUseEver);
        if (ImGui::Begin("サービス連携の診断", &open))
            m_serviceDiagnosticsPanel.Draw(m_onlineServices, m_projectSettings.online);
        ImGui::End();
    }
}
