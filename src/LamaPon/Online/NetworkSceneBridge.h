#pragma once

#include "LamaPon/Online/NetworkSession.h"

#include <memory>

namespace LamaPon
{
    class Scene;
    class GameObject;

    // SceneのポインターをGame Moduleに保持させず、Runtimeが同期を所有します。
    class NetworkSceneBridge final
    {
    public:
        LAMAPON_API NetworkSceneBridge(Scene& scene, NetworkSession& session);
        LAMAPON_API ~NetworkSceneBridge();
        NetworkSceneBridge(const NetworkSceneBridge&) = delete;
        NetworkSceneBridge& operator=(const NetworkSceneBridge&) = delete;
        LAMAPON_API void BeforeSimulation(float elapsedSeconds);
        LAMAPON_API void AfterSimulation(float elapsedSeconds);
        // 接続を止め、参加者用に無効化したComponentを復元します。
        LAMAPON_API void Reset();
        [[nodiscard]] LAMAPON_API GameObject* Spawn(std::string_view prefabKey,
            const NetworkTransform& transform = {}, NetworkPeerId owner = 1);
        LAMAPON_API bool Despawn(NetworkObjectId id);
        [[nodiscard]] LAMAPON_API GameObject* Find(NetworkObjectId id) const noexcept;

    private:
        struct Implementation;
        std::unique_ptr<Implementation> m_impl;
    };

    [[nodiscard]] LAMAPON_API NetworkSceneBridge* ActiveNetworkSceneBridge() noexcept;
    LAMAPON_API void SetActiveNetworkSceneBridge(NetworkSceneBridge* bridge) noexcept;
}
