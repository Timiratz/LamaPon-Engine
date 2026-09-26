#include "LamaPon/Editor/NetworkConnectionPanel.h"
#include "LamaPon/Online/NetworkRoomBrowser.h"
#include "LamaPon/Online/NetworkSceneBridge.h"
#include <imgui.h>
#include <algorithm>
#include <array>

namespace LamaPon
{
    struct NetworkConnectionPanel::Implementation final
    {
        NetworkRoomBrowser browser;
        NetworkConfiguration searched;
        std::array<char, 513> joinAddress{};
        std::array<char, 33> playerName{ "Player" };
        std::array<char, 65> listenAddress{ "127.0.0.1" };
        std::array<char, 97> publicEndpoint{};
        std::array<char, 65> accessKey{};
    };
    NetworkConnectionPanel::NetworkConnectionPanel() : m_impl(std::make_unique<Implementation>()) {}
    NetworkConnectionPanel::~NetworkConnectionPanel() = default;
    void NetworkConnectionPanel::StopSearch() { m_impl->browser.Stop(); }
    void NetworkConnectionPanel::Draw(NetworkSession* session, NetworkSceneBridge* bridge,
        const bool playing, const NetworkConfiguration& configuration)
    {
        auto& ui = *m_impl;
        const bool active = session && session->State() != NetworkState::Stopped && session->State() != NetworkState::Error;
        if (!playing || (ui.browser.IsSearching()
            && (ui.searched.gameId != configuration.gameId || ui.searched.gameVersion != configuration.gameVersion
                || ui.searched.sceneId != configuration.sceneId || ui.searched.backend != configuration.backend
                || ui.searched.discoveryPort != configuration.discoveryPort))) ui.browser.Stop();
        ImGui::SeparatorText("接続の動作確認");
        ImGui::TextWrapped("エディター再生中に接続できます。設定を保存すると、次回の再生と配布ゲームにも反映されます。");
        ImGui::BeginDisabled(!playing || active || !session);
        ImGui::InputText("プレイヤー名", ui.playerName.data(), ui.playerName.size());
        if (configuration.backend != NetworkBackend::EpicOnlineServices)
        {
            ImGui::InputText("ホストの待受先", ui.listenAddress.data(), ui.listenAddress.size());
            if (ImGui::SmallButton("このPCだけ")) strcpy_s(ui.listenAddress.data(), ui.listenAddress.size(), "127.0.0.1");
            ImGui::SameLine();
            if (ImGui::SmallButton("IPv4で外部から受付")) strcpy_s(ui.listenAddress.data(), ui.listenAddress.size(), "0.0.0.0");
            ImGui::TextWrapped("127.0.0.1は同じPC、0.0.0.0はIPv4の外部接続、::はIPv6の外部接続を受け付けます。別回線ではポート転送または到達可能なIPv6が必要です。");
        }
        if (ImGui::Button("部屋を作成") && session && session->Configure(configuration))
            static_cast<void>(session->Host(ui.playerName.data(), ui.listenAddress.data()));
        ImGui::InputText("接続情報 / 接続先 / EOS部屋ID", ui.joinAddress.data(), ui.joinAddress.size());
        if (configuration.backend == NetworkBackend::Direct)
            ImGui::InputText("アクセスキー（接続先を別に指定する場合）", ui.accessKey.data(), ui.accessKey.size(), ImGuiInputTextFlags_Password);
        if (ImGui::Button("部屋に参加") && session && session->Configure(configuration))
        {
            if (configuration.backend == NetworkBackend::Direct && ui.accessKey[0] != '\0')
                static_cast<void>(session->JoinDirect(ui.joinAddress.data(), ui.accessKey.data(), ui.playerName.data()));
            else static_cast<void>(session->Join(ui.joinAddress.data(), ui.playerName.data()));
            std::fill(ui.accessKey.begin(), ui.accessKey.end(), '\0');
        }
        ImGui::EndDisabled();
        if (configuration.backend != NetworkBackend::EpicOnlineServices)
        {
            ImGui::BeginDisabled(!playing || active || !session);
            if (ImGui::Button("LANの部屋を検索")) { ui.searched = configuration; static_cast<void>(ui.browser.Start(configuration)); }
            ImGui::SameLine();
            if (ImGui::Button("このPCの部屋を検索"))
            { ui.searched = configuration; static_cast<void>(ui.browser.Start(configuration, NetworkDiscoveryScope::SameComputer)); }
            ImGui::EndDisabled();
            if (ui.browser.IsSearching())
            {
                ui.browser.Update(ImGui::GetIO().DeltaTime);
                if (ImGui::SmallButton("検索を終了")) ui.browser.Stop();
                int roomIndex{};
                for (const auto& room : ui.browser.Rooms())
                {
                    ImGui::PushID(roomIndex++);
                    ImGui::Text("%s (%u/%u)", room.name.c_str(), room.players, room.capacity);
                    ImGui::SameLine(); ImGui::BeginDisabled(active || !session || room.players >= room.capacity);
                    if (ImGui::SmallButton("参加") && session && session->Configure(configuration))
                        static_cast<void>(session->JoinRoom(room, ui.playerName.data()));
                    ImGui::EndDisabled(); ImGui::PopID();
                }
            }
            if (!ui.browser.LastError().empty()) ImGui::TextWrapped("%s", ui.browser.LastError().c_str());
        }
        if (session)
        {
            ImGui::Text("状態: %s", NetworkStateName(session->State()).data());
            if (!session->ConnectionStatus().empty()) ImGui::TextWrapped("%s", session->ConnectionStatus().c_str());
            if (session->IsHost() && session->Configuration().backend == NetworkBackend::Direct)
            {
                ImGui::InputText("相手に伝える接続先（任意）", ui.publicEndpoint.data(), ui.publicEndpoint.size());
                ImGui::TextWrapped("LAN IPv4・公開IPv4・IPv6を指定して接続情報を作れます。空欄では自動設定の公開先、またはこのPC用の接続先を使います。秘密部分を含むため、参加者だけへ共有してください。");
            }
            const auto room = session->Configuration().backend == NetworkBackend::Direct
                ? session->ConnectionCode(ui.publicEndpoint.data()) : session->RoomAddress();
            if (!room.empty())
            {
                if (session->Configuration().backend != NetworkBackend::Direct) ImGui::TextWrapped("部屋: %s", room.c_str());
                if (ImGui::SmallButton("接続情報をコピー")) ImGui::SetClipboardText(room.c_str());
            }
            for (const auto& member : session->Members())
                ImGui::BulletText("%u: %s", member.id, member.name.c_str());
            const auto statistics = session->Statistics();
            ImGui::Text("往復: %.1fms / 同期オブジェクト: %zu", statistics.roundTripMilliseconds, session->Objects().size());
            if (!session->LastError().empty()) ImGui::TextWrapped("%s", session->LastError().c_str());
            if (active && ImGui::Button("接続を終了"))
            {
                if (bridge) bridge->Reset();
                else session->Stop();
            }
        }
        ImGui::Spacing();
    }
}
