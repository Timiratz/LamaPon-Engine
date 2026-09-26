#include "LamaPon/Components/NetworkIdentityComponent.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace LamaPon
{
    NetworkIdentityComponent::NetworkIdentityComponent(std::string sceneKey)
    {
        SetSceneKey(std::move(sceneKey));
    }
    void NetworkIdentityComponent::SetSceneKey(std::string key)
    {
        if (key.size() > 64 || !std::ranges::all_of(key, [](const char c)
            { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'; }))
            throw std::invalid_argument("Scene Keyは64文字以内の英数字・._-です。");
        m_sceneKey = std::move(key);
    }
    bool NetworkIdentityComponent::IsLocalOwner() const noexcept
    {
        const auto* session = ActiveNetworkSession();
        return session && m_id != 0 && session->LocalPeer() == m_ownerPeer;
    }
    void NetworkIdentityComponent::SetInterpolationSeconds(const float seconds)
    {
        if (!std::isfinite(seconds) || seconds < 0 || seconds > 1)
            throw std::invalid_argument("補間時間は0〜1秒です。");
        m_interpolation = seconds;
    }
    void NetworkIdentityComponent::SetReplicatedData(std::string data)
    {
        if (data.size() > 256) throw std::invalid_argument("同期データは256バイト以内です。");
        m_data = std::move(data);
    }
    void NetworkIdentityComponent::BindNetworkId(const NetworkObjectId id,
        const NetworkPeerId owner) noexcept
    {
        m_id = id; m_ownerPeer = owner;
    }
}
