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
        int backend = m_projectNetworkDraft.backend == NetworkBackend::Lan ? 0 : 1;
        if (ImGui::Combo("接続方式", &backend, "LAN / 同じPC\0インターネット（EOS）\0"))
            m_projectNetworkDraft.backend = backend == 0 ? NetworkBackend::Lan : NetworkBackend::EpicOnlineServices;
        EditNetworkText("ゲームID##P2P", m_projectNetworkDraft.gameId);
        EditNetworkText("通信バージョン", m_projectNetworkDraft.gameVersion);
        EditNetworkText("シーンID", m_projectNetworkDraft.sceneId);
        int players = static_cast<int>(m_projectNetworkDraft.maxPlayers);
        if (ImGui::SliderInt("最大人数（ホストを含む）", &players, 2, 4))
            m_projectNetworkDraft.maxPlayers = static_cast<std::uint32_t>(players);
        int rate = static_cast<int>(m_projectNetworkDraft.tickRate);
        if (ImGui::SliderInt("状態送信の頻度（Hz）", &rate, 1, 60))
            m_projectNetworkDraft.tickRate = static_cast<std::uint32_t>(rate);
        ImGui::SliderFloat("切断待ち時間（秒）", &m_projectNetworkDraft.timeoutSeconds, 5, 120);
        if (m_projectNetworkDraft.backend == NetworkBackend::Lan)
        {
            int port = m_projectNetworkDraft.port;
            if (ImGui::InputInt("待受ポート", &port))
                m_projectNetworkDraft.port = static_cast<std::uint16_t>(std::clamp(port, 0, 65535));
            ImGui::TextWrapped("LAN方式は同じネットワーク内の信頼できる参加者向けです。インターネットで遊ぶ場合はEOSを選択してください。ポート0は空いているポートを自動選択します。");
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

        ImGui::SeparatorText("接続の動作確認");
        ImGui::TextWrapped("エディター再生中に接続できます。設定を保存すると、次回の再生と配布ゲームにも反映されます。");
        ImGui::BeginDisabled(!m_playing || active || !session);
        ImGui::InputText("プレイヤー名", m_networkPlayerName.data(), m_networkPlayerName.size());
        if (m_projectNetworkDraft.backend == NetworkBackend::Lan)
        {
            ImGui::InputText("ホストの待受IPv4", m_networkListenAddress.data(), m_networkListenAddress.size());
            ImGui::TextWrapped("127.0.0.1は同じPCだけ、0.0.0.0はLANからの参加も受け付けます。LANの相手にはホストPCのLAN IPv4を伝えてください。");
        }
        if (ImGui::Button("部屋を作成") && session && session->Configure(m_projectNetworkDraft))
            static_cast<void>(session->Host(m_networkPlayerName.data(), m_networkListenAddress.data()));
        ImGui::InputText("接続先（IPv4:port / EOS部屋ID）", m_networkJoinAddress.data(), m_networkJoinAddress.size());
        if (ImGui::Button("部屋に参加") && session && session->Configure(m_projectNetworkDraft))
            static_cast<void>(session->Join(m_networkJoinAddress.data(), m_networkPlayerName.data()));
        ImGui::EndDisabled();
        if (session)
        {
            ImGui::Text("状態: %s", NetworkStateName(session->State()).data());
            const auto room = session->RoomAddress();
            if (!room.empty())
            {
                ImGui::TextWrapped("部屋: %s", room.c_str());
                if (ImGui::SmallButton("部屋IDをコピー")) ImGui::SetClipboardText(room.c_str());
            }
            for (const auto& member : session->Members())
                ImGui::BulletText("%u: %s", member.id, member.name.c_str());
            const auto statistics = session->Statistics();
            ImGui::Text("往復: %.1fms / 同期オブジェクト: %zu", statistics.roundTripMilliseconds, session->Objects().size());
            if (!session->LastError().empty()) ImGui::TextWrapped("%s", session->LastError().c_str());
            if (active && ImGui::Button("接続を終了"))
            {
                if (auto* bridge = ActiveNetworkSceneBridge()) bridge->Reset();
                else session->Stop();
            }
        }
        ImGui::Spacing();
    }
}
