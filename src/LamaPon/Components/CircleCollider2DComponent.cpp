#include "LamaPon/Components/CircleCollider2DComponent.h"

#include "LamaPon/Graphics/DebugRenderer.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Physics/CollisionTypes.h"
#include "LamaPon/Scene/GameObject.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace LamaPon
{
    CircleCollider2DComponent::CircleCollider2DComponent(
        const float radius,
        const DirectX::XMFLOAT2 offset,
        const bool isTrigger,
        const std::uint32_t layer,
        const std::uint32_t collisionMask,
        PhysicsMaterial material) noexcept
        : m_radius(std::max(radius, 0.001f))
        , m_offset(offset)
        , m_isTrigger(isTrigger)
        , m_layer(layer % 32u)
        , m_collisionMask(collisionMask)
        , m_material(material)
    {
        m_material.Clamp();
    }

    void CircleCollider2DComponent::SetRadius(
        const float radius) noexcept
    {
        m_radius = std::max(radius, 0.001f);
    }

    Circle2D
        CircleCollider2DComponent::WorldCircle()
            const noexcept
    {
        using namespace DirectX;
        const XMMATRIX world = Owner().WorldMatrix();
        XMFLOAT3 center{};
        XMStoreFloat3(
            &center,
            XMVector3TransformCoord(
                XMVectorSet(
                    m_offset.x,
                    m_offset.y,
                    0.0f,
                    1.0f),
                world));
        // 非一様スケールは大きい方の軸で近似します。
        const float scaleX = XMVectorGetX(
            XMVector3Length(
                XMVector3TransformNormal(
                    XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f),
                    world)));
        const float scaleY = XMVectorGetX(
            XMVector3Length(
                XMVector3TransformNormal(
                    XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f),
                    world)));
        return {
            { center.x, center.y },
            m_radius * std::max(scaleX, scaleY)
        };
    }

    Bounds2D
        CircleCollider2DComponent::WorldBounds()
            const noexcept
    {
        const auto circle = WorldCircle();
        return {
            {
                circle.center.x - circle.radius,
                circle.center.y - circle.radius
            },
            {
                circle.center.x + circle.radius,
                circle.center.y + circle.radius
            }
        };
    }

    void CircleCollider2DComponent::OnRenderDebug3D(
        GraphicsDevice& graphics,
        DirectX::FXMMATRIX view,
        DirectX::CXMMATRIX projection)
    {
        const auto circle = WorldCircle();
        constexpr int SegmentCount = 24;
        std::array<
            DirectX::XMFLOAT3,
            SegmentCount * 2> lines{};
        for (int segment = 0;
            segment < SegmentCount;
            ++segment)
        {
            const float angleA =
                DirectX::XM_2PI
                * static_cast<float>(segment)
                / static_cast<float>(SegmentCount);
            const float angleB =
                DirectX::XM_2PI
                * static_cast<float>(segment + 1)
                / static_cast<float>(SegmentCount);
            lines[segment * 2] = {
                circle.center.x
                    + std::cos(angleA) * circle.radius,
                circle.center.y
                    + std::sin(angleA) * circle.radius,
                0.0f };
            lines[segment * 2 + 1] = {
                circle.center.x
                    + std::cos(angleB) * circle.radius,
                circle.center.y
                    + std::sin(angleB) * circle.radius,
                0.0f };
        }
        const DirectX::XMVECTOR color = m_isTrigger
            ? DirectX::XMVectorSet(
                1.0f, 0.75f, 0.1f, 1.0f)
            : DirectX::XMVectorSet(
                0.1f, 1.0f, 0.35f, 1.0f);
        graphics.Debug().DrawLines(
            lines,
            color,
            view,
            projection);
    }
}
