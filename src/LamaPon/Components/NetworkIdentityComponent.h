#pragma once

#include "LamaPon/Online/NetworkSession.h"
#include "LamaPon/Scene/Component.h"

namespace LamaPon
{
    // Scene上の固定オブジェクトには、双方で同じ一意なScene Keyを指定します。
    // PrefabのScene Keyは空です。通信IDと所有者は保存せず、接続中だけ有効です。
    class NetworkIdentityComponent final : public Component
    {
    public:
        explicit NetworkIdentityComponent(std::string sceneKey = {});
        [[nodiscard]] std::string_view TypeName() const noexcept override { return "NetworkIdentity"; }
        [[nodiscard]] const std::string& SceneKey() const noexcept { return m_sceneKey; }
        void SetSceneKey(std::string key);
        [[nodiscard]] NetworkObjectId NetworkId() const noexcept { return m_id; }
        [[nodiscard]] NetworkPeerId OwnerPeer() const noexcept { return m_ownerPeer; }
        [[nodiscard]] bool IsLocalOwner() const noexcept;
        [[nodiscard]] bool HostOnlySimulation() const noexcept { return m_hostOnly; }
        void SetHostOnlySimulation(bool value) noexcept { m_hostOnly = value; }
        [[nodiscard]] float InterpolationSeconds() const noexcept { return m_interpolation; }
        void SetInterpolationSeconds(float seconds);
        [[nodiscard]] const std::string& ReplicatedData() const noexcept { return m_data; }
        void SetReplicatedData(std::string data);
        // RuntimeのScene Bridgeが使います。ScriptではIDを直接設定せず、
        // NetworkSpawn / NetworkDespawnを使用してください。
        void BindNetworkId(NetworkObjectId id, NetworkPeerId owner) noexcept;

    private:
        std::string m_sceneKey;
        NetworkObjectId m_id{};
        NetworkPeerId m_ownerPeer{ 1 };
        bool m_hostOnly{ true };
        float m_interpolation{ 0.1f };
        std::string m_data;
    };
}
