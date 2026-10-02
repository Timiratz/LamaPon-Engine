#include "LamaPon/Components/CameraComponent.h"

#include "LamaPon/Scene/GameObject.h"
#include "LamaPon/Scene/Transform.h"

namespace LamaPon
{
    CameraComponent::CameraComponent(
        const float verticalFieldOfView,
        const float nearPlane,
        const float farPlane) noexcept
        : m_verticalFieldOfView(verticalFieldOfView)
        , m_nearPlane(nearPlane)
        , m_farPlane(farPlane)
    {
    }

    DirectX::XMMATRIX CameraComponent::ViewMatrix() const noexcept
    {
        using namespace DirectX;

        // カメラ所有物体のワールド変換
        const XMMATRIX world = Owner().WorldMatrix();
        // カメラのワールド位置
        const XMVECTOR position = world.r[3];
        // ワールドでのカメラ前向き
        const XMVECTOR forward = XMVector3Normalize(XMVectorNegate(world.r[2]));
        // ワールドでのカメラ上向き
        const XMVECTOR up = XMVector3Normalize(world.r[1]);

        return XMMatrixLookToRH(position, forward, up);
    }

    DirectX::XMMATRIX CameraComponent::ProjectionMatrix(const float aspectRatio) const noexcept
    {
        return DirectX::XMMatrixPerspectiveFovRH(
            m_verticalFieldOfView,
            aspectRatio,
            m_nearPlane,
            m_farPlane);
    }
}
