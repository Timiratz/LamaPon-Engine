#include "LamaPon/Components/ReflectionProbeComponent.h"

#include "LamaPon/Scene/GameObject.h"

#include <algorithm>
#include <cmath>

namespace LamaPon
{
    DirectX::XMFLOAT3
        ReflectionProbeComponent::WorldPosition()
        const noexcept
    {
        // プローブのワールド位置
        DirectX::XMFLOAT3 position{};
        DirectX::XMStoreFloat3(
            &position,
            Owner().WorldMatrix().r[3]);
        return position;
    }

    float ReflectionProbeComponent::InfluenceAt(
        const DirectX::XMFLOAT3& position) const noexcept
    {
        // プローブのワールド中心
        const auto center = WorldPosition();
        // 中心からのX距離
        const float dx = position.x - center.x;
        // 中心からのY距離
        const float dy = position.y - center.y;
        // 中心からのZ距離
        const float dz = position.z - center.z;
        // 中心からのワールド距離
        const float distance = std::sqrt(
            dx * dx + dy * dy + dz * dz);
        if (distance > m_range)
        {
            return 0.0f;
        }
        // 混合距離0では範囲内を一律1とし、混合域では縁へ向けて0に下げます。
        // 影響半径以下の混合距離
        const float blend = BlendDistance();
        if (blend <= 0.0f)
        {
            return 1.0f;
        }
        return std::clamp(
            (m_range - distance) / blend,
            0.0f,
            1.0f);
    }
}
