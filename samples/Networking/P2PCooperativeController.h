#pragma once

#include "LamaPon/LamaPon.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace LamaPon::Samples
{
    // player PrefabのrootにNetworkIdentityを付け、管理Scriptをその子に置かず常時有効に保つ。
    class P2PCooperativeController final
    {
    public:
        // 各フレームに入力を送ってホストが所有者と移動範囲を検証する(session: 通信の状態とイベント源, bridge: 同期Objectの生成と検索, seconds: 前回からの秒数, horizontal: ローカル入力のX軸値, vertical: ローカル入力のZ軸値)。
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
            // 0.1秒を上限にする更新間隔
            const float dt = std::min(seconds, 0.1f);
            if (session.IsHost())
            {
                // 消えたObjectの参加者登録を除く(player: 参加者IDとObject IDの組)。
                std::erase_if(m_players, [&session](const auto& player)
                    { return session.FindObject(player.second) == nullptr; });
                // playerを生成する参加者情報
                for (const auto& member : session.Members())
                {
                    if (m_players.contains(member.id)) continue;
                    // 4列に並べる初期同期位置
                    NetworkTransform transform;
                    transform.position[0] = static_cast<float>((member.id - 1) % 4) * 2;
                    // 参加者所有で生成したGameObject
                    if (auto* object = bridge.Spawn("player", transform, member.id))
                        m_players.emplace(member.id, object->GetComponent<NetworkIdentityComponent>()->NetworkId());
                }
            }
            m_sendTime += dt;
            if (m_sendTime >= 0.05f)
            {
                m_sendTime = 0;
                // x: X入力の-1～1、z: Z入力の-1～1
                const int x = Axis(horizontal), z = Axis(vertical);
                // 2軸入力を0～8に詰めた文字
                const char direction = static_cast<char>('0' + (z + 1) * 3 + x + 1);
                // 入力を送る所有Objectの同期情報
                for (const auto& object : session.Objects())
                    if (object.prefabKey == "player" && object.owner == session.LocalPeer())
                        static_cast<void>(session.SendInput(object.id, "move", std::string(1, direction)));
            }
            // このUpdateが全イベントを消費するため、他Scriptへ渡す場合はここで配る。
            // Sessionから取り出す受信イベント
            NetworkEvent event;
            while (session.PollEvent(event))
            {
                if (!session.IsHost() || event.kind != NetworkEventKind::Input || event.name != "move") continue;
                // 送信者の所有権を照合する同期Object
                const auto* object = session.FindObject(event.object);
                if (!object || object->prefabKey != "player" || object->owner != event.peer
                    || event.data.size() != 1 || event.data[0] < '0' || event.data[0] > '8') continue;
                // 文字から戻した0～8の入力番号
                const int direction = event.data[0] - '0';
                m_inputs[event.object] = { direction % 3 - 1, direction / 3 - 1, 0 };
            }
            if (!session.IsHost()) return;
            // 消えたObjectの入力記録を除く(input: Object IDと受信入力の組)。
            std::erase_if(m_inputs, [&session](const auto& input) { return session.FindObject(input.first) == nullptr; });
            // id: 同期Object ID、input: 受信入力と経過秒
            for (auto& [id, input] : m_inputs)
            {
                input.age += dt;
                if (input.age > 0.3f) continue;
                // ホスト側で移動させるGameObject
                auto* object = bridge.Find(id);
                if (!object) continue;
                // 速度・更新秒・移動範囲はホストで決め、0.3秒を超えた入力を移動へ使わない。
                // 斜め移動の長さを補正する倍率
                const float scale = input.x != 0 && input.z != 0 ? 0.70710678f : 1;
                // ホストが範囲内へ更新するWorld位置
                auto& position = object->GetTransform().position;
                position.x = std::clamp(position.x + static_cast<float>(input.x) * scale * 3 * dt, -25.0f, 25.0f);
                position.z = std::clamp(position.z + static_cast<float>(input.z) * scale * 3 * dt, -25.0f, 25.0f);
            }
        }

    private:
        // 非有限値と±0.2内を0にし、それ以外を-1か1へ量子化する(value: ローカル入力の軸値)。
        static int Axis(const float value)
        { return !std::isfinite(value) ? 0 : value > 0.2f ? 1 : value < -0.2f ? -1 : 0; }
        // ホストが保持する2軸入力と受信後の経過秒
        struct Input final
        {
            // 受信したX方向の-1～1
            int x{};
            // 受信したZ方向の-1～1
            int z{};
            // 最終入力からの経過秒
            float age{};
        };
        // 前回更新したSessionの世代
        std::uint64_t m_generation{};
        // 0.05秒間隔の入力送信待ち秒
        float m_sendTime{};
        // 参加者IDから生成済みObject ID
        std::map<NetworkPeerId, NetworkObjectId> m_players;
        // Objectごとの最終受信入力
        std::map<NetworkObjectId, Input> m_inputs;
    };
}
