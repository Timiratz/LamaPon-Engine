#include "LamaPon/Components/SphereCollider3DComponent.h"

#include "LamaPon/Graphics/DebugRenderer.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Scene/GameObject.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace LamaPon
{
    SphereCollider3DComponent::
        SphereCollider3DComponent(
            const float radius,
            const DirectX::XMFLOAT3 offset,
            const bool isTrigger,
            const std::uint32_t layer,
            const std::uint32_t collisionMask,
            PhysicsMaterial material) noexcept
        : m_radius(std::max(radius, 0.01f))
        , m_offset(offset)
        , m_isTrigger(isTrigger)
        , m_layer(layer % 32u)
        , m_collisionMask(collisionMask)
        , m_material(material)
    {
        m_material.Clamp();
    }

    void SphereCollider3DComponent::SetRadius(
        const float value) noexcept
    {
        m_radius = std::max(value, 0.01f);
    }

    Sphere3D
        SphereCollider3DComponent::WorldSphere()
            const noexcept
    {
        using namespace DirectX;
        const auto world = Owner().WorldMatrix();
        XMFLOAT3 center{};
        XMStoreFloat3(
            &center,
            XMVector3TransformCoord(
                XMLoadFloat3(&m_offset),
                world));
        const float scaleX = XMVectorGetX(
            XMVector3Length(
                XMVector3TransformNormal(
                    XMVectorSet(
                        1.0f,
                        0.0f,
                        0.0f,
                        0.0f),
                    world)));
        const float scaleY = XMVectorGetX(
            XMVector3Length(
                XMVector3TransformNormal(
                    XMVectorSet(
                        0.0f,
                        1.0f,
                        0.0f,
                        0.0f),
                    world)));
        const float scaleZ = XMVectorGetX(
            XMVector3Length(
                XMVector3TransformNormal(
                    XMVectorSet(
                        0.0f,
                        0.0f,
                        1.0f,
                        0.0f),
                    world)));
        return {
            center,
            m_radius
                * std::max({
                    scaleX,
                    scaleY,
                    scaleZ
                })
        };
    }

    Bounds3D
        SphereCollider3DComponent::WorldBounds()
            const noexcept
    {
        return BoundsOf(WorldSphere());
    }

    void SphereCollider3DComponent::OnRenderDebug3D(
        GraphicsDevice& graphics,
        DirectX::FXMMATRIX view,
        DirectX::CXMMATRIX projection)
    {
        using namespace DirectX;
        const auto sphere = WorldSphere();
        const auto color = m_isTrigger
            ? XMVectorSet(
                1.0f,
                0.75f,
                0.1f,
                1.0f)
            : XMVectorSet(
                0.35f,
                0.9f,
                0.55f,
                1.0f);
        constexpr int segments = 24;
        std::vector<XMFLOAT3> lines;
        lines.reserve(segments * 6);
        for (int plane{}; plane < 3; ++plane)
        {
            for (int index{}; index < segments;
                ++index)
            {
                const float firstAngle =
                    XM_2PI
                    * static_cast<float>(index)
                    / static_cast<float>(segments);
                const float secondAngle =
                    XM_2PI
                    * static_cast<float>(index + 1)
                    / static_cast<float>(segments);
                const auto point =
                    [&](const float angle)
                {
                    XMFLOAT3 result = sphere.center;
                    const float first =
                        std::cos(angle)
                        * sphere.radius;
                    const float second =
                        std::sin(angle)
                        * sphere.radius;
                    if (plane == 0)
                    {
                        result.x += first;
                        result.y += second;
                    }
                    else if (plane == 1)
                    {
                        result.x += first;
                        result.z += second;
                    }
                    else
                    {
                        result.y += first;
                        result.z += second;
                    }
                    return result;
                };
                lines.push_back(point(firstAngle));
                lines.push_back(point(secondAngle));
            }
        }
        graphics.Debug().DrawLines(
            lines,
            color,
            view,
            projection);
    }
}
