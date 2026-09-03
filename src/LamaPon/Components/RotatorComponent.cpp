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
        // 回転の合成はクォータニオンで行います。オイラー角の成分を
        // 足す書き方だと、軸が絡んだときに順序依存で誤差が溜まり、
        // ピッチ90度付近では意図しない向きへ跳ねます。
        GetTransform().RotateEuler({
            m_angularVelocity.x * deltaTime,
            m_angularVelocity.y * deltaTime,
            m_angularVelocity.z * deltaTime
        });
    }
}
