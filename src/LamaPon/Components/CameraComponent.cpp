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

        const XMMATRIX world = Owner().WorldMatrix();
        const XMVECTOR position = world.r[3];
        const XMVECTOR forward = XMVector3Normalize(XMVectorNegate(world.r[2]));
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
