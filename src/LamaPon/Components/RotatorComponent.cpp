#include "LamaPon/Components/RotatorComponent.h"

#include "LamaPon/Scene/Transform.h"

namespace LamaPon
{
    RotatorComponent::RotatorComponent(
        const DirectX::XMFLOAT3 angularVelocity) noexcept
        : m_angularVelocity(angularVelocity)
    {
    }

    void RotatorComponent::OnUpdate(const float deltaTime)
    {
        // オイラー増分をクォータニオンに変換して合成します。
        GetTransform().RotateEuler({
            m_angularVelocity.x * deltaTime,
            m_angularVelocity.y * deltaTime,
            m_angularVelocity.z * deltaTime
        });
    }
}
