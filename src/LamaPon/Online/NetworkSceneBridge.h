#pragma once

#include "LamaPon/Online/NetworkSession.h"

#include <memory>

namespace LamaPon
{
    class Scene;
    class GameObject;

    // SceneのポインターをGame Moduleに保持させず、Runtimeが同期を所有します。
    // sceneとsessionはこの同期処理より長く生存し、破棄前にResetで接続状態を復元します。
    class NetworkSceneBridge final
    {
    public:
        // シーンと通信状態の対応を作る(scene: 借用するシーン, session: 借用する通信状態)。
        LAMAPON_API NetworkSceneBridge(Scene& scene, NetworkSession& session);
        // 対応状態を破棄し、アクティブ登録を解除する。
        LAMAPON_API ~NetworkSceneBridge();
        // シーン同期の複製を禁止する。
        NetworkSceneBridge(const NetworkSceneBridge&) = delete;
        // シーン同期のコピー代入を禁止する。
        NetworkSceneBridge& operator=(const NetworkSceneBridge&) = delete;
        // 参加側の状態と補間をシミュレーション前に反映する(elapsedSeconds: 経過秒数)。
        LAMAPON_API void BeforeSimulation(float elapsedSeconds);
        // シミュレーション後のホスト状態を次の送信に登録する(elapsedSeconds: 経過秒数)。
        LAMAPON_API void AfterSimulation(float elapsedSeconds);
        // 接続を止め、固定オブジェクトと無効化したComponentを復元する。
        LAMAPON_API void Reset();
        // ホストで同期Prefabを作り、失敗時は空を返す(prefabKey: 登録済みPrefabの識別子, transform: 初期変換, owner: 入力の所有者ID)。
        [[nodiscard]] LAMAPON_API GameObject* Spawn(std::string_view prefabKey,
            const NetworkTransform& transform = {}, NetworkPeerId owner = 1);
        // ホストへ同期対象の削除を登録する(id: 通信オブジェクトID)。
        LAMAPON_API bool Despawn(NetworkObjectId id);
        // 対応するシーン内オブジェクトを借用する(id: 通信オブジェクトID)。
        [[nodiscard]] LAMAPON_API GameObject* Find(NetworkObjectId id) const noexcept;

    private:
        struct Implementation;
        // シーン同期状態の所有先
        std::unique_ptr<Implementation> m_impl;
    };

    // 現在のシーン同期を借用する。
    [[nodiscard]] LAMAPON_API NetworkSceneBridge* ActiveNetworkSceneBridge() noexcept;
    // 所有権を持たずにアクティブ同期を登録する(bridge: 登録する同期・空で解除)。
    LAMAPON_API void SetActiveNetworkSceneBridge(NetworkSceneBridge* bridge) noexcept;
}
