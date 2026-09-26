#pragma once

#include "LamaPon/LamaPon.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace LamaPon::Samples
{
    // Sceneの常時有効な管理Scriptから一度だけ呼び出します。
    // playerという同期Prefabを登録し、rootにNetworkIdentityを付けてください。
    // この管理Script自身をNetworkIdentityの子に置くと、参加者で無効になります。
    class P2PCooperativeController final
    {
    public:
        void Update(NetworkSession& session, NetworkSceneBridge& bridge,
            const float seconds, const float horizontal, const float vertical)
        {
            if (m_generation != session.Generation())
            {
                m_generation = session.Generation();
                m_players.clear(); m_inputs.clear(); m_sendTime = 0;
            }
            if (!session.IsHost() && session.State() != NetworkState::Connected) return;
            if (!std::isfinite(seconds) || seconds < 0) return;
            const float dt = std::min(seconds, 0.1f);
            if (session.IsHost())
            {
                std::erase_if(m_players, [&session](const auto& player)
                    { return session.FindObject(player.second) == nullptr; });
                for (const auto& member : session.Members())
                {
                    if (m_players.contains(member.id)) continue;
                    NetworkTransform transform;
                    transform.position[0] = static_cast<float>((member.id - 1) % 4) * 2;
                    if (auto* object = bridge.Spawn("player", transform, member.id))
                        m_players.emplace(member.id, object->GetComponent<NetworkIdentityComponent>()->NetworkId());
                }
            }
            m_sendTime += dt;
            if (m_sendTime >= 0.05f)
            {
                m_sendTime = 0;
                const int x = Axis(horizontal), z = Axis(vertical);
                const char direction = static_cast<char>('0' + (z + 1) * 3 + x + 1);
                for (const auto& object : session.Objects())
                    if (object.prefabKey == "player" && object.owner == session.LocalPeer())
                        static_cast<void>(session.SendInput(object.id, "move", std::string(1, direction)));
            }
            // イベントを取り出す場所は一つにまとめ、必要なら他のScriptへ配ります。
            NetworkEvent event;
            while (session.PollEvent(event))
            {
                if (!session.IsHost() || event.kind != NetworkEventKind::Input || event.name != "move") continue;
                const auto* object = session.FindObject(event.object);
                if (!object || object->prefabKey != "player" || object->owner != event.peer
                    || event.data.size() != 1 || event.data[0] < '0' || event.data[0] > '8') continue;
                const int direction = event.data[0] - '0';
                m_inputs[event.object] = { direction % 3 - 1, direction / 3 - 1, 0 };
            }
            if (!session.IsHost()) return;
            std::erase_if(m_inputs, [&session](const auto& input) { return session.FindObject(input.first) == nullptr; });
            for (auto& [id, input] : m_inputs)
            {
                input.age += dt;
                if (input.age > 0.3f) continue;
                auto* object = bridge.Find(id);
                if (!object) continue;
                // 速度・経過時間・移動範囲はホストが決め、座標は受信しません。
                const float scale = input.x != 0 && input.z != 0 ? 0.70710678f : 1;
                auto& position = object->GetTransform().position;
                position.x = std::clamp(position.x + static_cast<float>(input.x) * scale * 3 * dt, -25.0f, 25.0f);
                position.z = std::clamp(position.z + static_cast<float>(input.z) * scale * 3 * dt, -25.0f, 25.0f);
            }
        }

    private:
        static int Axis(const float value)
        { return !std::isfinite(value) ? 0 : value > 0.2f ? 1 : value < -0.2f ? -1 : 0; }
        struct Input final { int x{}; int z{}; float age{}; };
        std::uint64_t m_generation{};
        float m_sendTime{};
        std::map<NetworkPeerId, NetworkObjectId> m_players;
        std::map<NetworkObjectId, Input> m_inputs;
    };
}
