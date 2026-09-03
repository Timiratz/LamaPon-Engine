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
        DirectX::XMFLOAT3 position{};
        DirectX::XMStoreFloat3(
            &position,
            Owner().WorldMatrix().r[3]);
        return position;
    }

    float ReflectionProbeComponent::InfluenceAt(
        const DirectX::XMFLOAT3& position) const noexcept
    {
        const auto center = WorldPosition();
        const float dx = position.x - center.x;
        const float dy = position.y - center.y;
        const float dz = position.z - center.z;
        const float distance = std::sqrt(
            dx * dx + dy * dy + dz * dz);
        if (distance > m_range)
        {
            return 0.0f;
        }
        // ブレンド距離0なら、範囲の内側は一律1（境界で切り替わる
        // 従来の挙動）。値があれば縁からその厚みで0へ落とします。
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
