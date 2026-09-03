#include "LamaPon/Components/CapsuleCollider3DComponent.h"

#include "LamaPon/Graphics/DebugRenderer.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Scene/GameObject.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace LamaPon
{
    CapsuleCollider3DComponent::CapsuleCollider3DComponent(
        const float radius,
        const float height,
        const DirectX::XMFLOAT3 offset,
        const bool isTrigger,
        const std::uint32_t layer,
        const std::uint32_t collisionMask,
        PhysicsMaterial material) noexcept
        : m_radius(std::max(radius, 0.01f))
        , m_height(std::max(height, m_radius * 2.0f))
        , m_offset(offset)
        , m_isTrigger(isTrigger)
        , m_layer(layer % 32u)
        , m_collisionMask(collisionMask)
        , m_material(material)
    {
        m_material.Clamp();
    }

    void CapsuleCollider3DComponent::SetRadius(
        const float value) noexcept
    {
        m_radius = std::max(value, 0.01f);
        m_height = std::max(m_height, m_radius * 2.0f);
    }

    void CapsuleCollider3DComponent::SetHeight(
        const float value) noexcept
    {
        m_height = std::max(value, m_radius * 2.0f);
    }

    Capsule3D CapsuleCollider3DComponent::WorldCapsule() const noexcept
    {
        using namespace DirectX;
        const auto world = Owner().WorldMatrix();
        XMFLOAT3 center{};
        XMStoreFloat3(
            &center,
            XMVector3TransformCoord(
                XMLoadFloat3(&m_offset),
                world));
        const auto worldX = XMVector3TransformNormal(
            XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f),
            world);
        const auto worldY = XMVector3TransformNormal(
            XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f),
            world);
        const auto worldZ = XMVector3TransformNormal(
            XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f),
            world);
        const float scaleX = XMVectorGetX(XMVector3Length(worldX));
        const float scaleY = XMVectorGetX(XMVector3Length(worldY));
        const float scaleZ = XMVectorGetX(XMVector3Length(worldZ));
        const float radius =
            m_radius * std::max(scaleX, scaleZ);
        const auto axis = scaleY > 0.000001f
            ? XMVectorScale(worldY, 1.0f / scaleY)
            : XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
        const float halfSegment =
            std::max(m_height * 0.5f - m_radius, 0.0f)
            * scaleY;
        XMFLOAT3 start{};
        XMFLOAT3 end{};
        XMStoreFloat3(
            &start,
            XMVectorSubtract(
                XMLoadFloat3(&center),
                XMVectorScale(axis, halfSegment)));
        XMStoreFloat3(
            &end,
            XMVectorAdd(
                XMLoadFloat3(&center),
                XMVectorScale(axis, halfSegment)));
        return { start, end, radius };
    }

    Bounds3D CapsuleCollider3DComponent::WorldBounds() const noexcept
    {
        return BoundsOf(WorldCapsule());
    }

    void CapsuleCollider3DComponent::OnRenderDebug3D(
        GraphicsDevice& graphics,
        DirectX::FXMMATRIX view,
        DirectX::CXMMATRIX projection)
    {
        using namespace DirectX;
        const auto color = m_isTrigger
            ? XMVectorSet(1.0f, 0.75f, 0.1f, 1.0f)
            : XMVectorSet(0.2f, 0.8f, 1.0f, 1.0f);
        const auto capsule = WorldCapsule();
        const XMVECTOR start = XMLoadFloat3(&capsule.start);
        const XMVECTOR end = XMLoadFloat3(&capsule.end);
        XMVECTOR axis = XMVectorSubtract(end, start);
        const float axisLength = XMVectorGetX(XMVector3Length(axis));
        axis = axisLength > 0.000001f
            ? XMVectorScale(axis, 1.0f / axisLength)
            : XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
        const XMVECTOR reference =
            std::abs(XMVectorGetY(axis)) < 0.9f
                ? XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f)
                : XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f);
        const XMVECTOR tangent =
            XMVector3Normalize(XMVector3Cross(axis, reference));
        const XMVECTOR bitangent =
            XMVector3Normalize(XMVector3Cross(axis, tangent));
        constexpr int segments = 16;
        std::vector<XMFLOAT3> lines;
        lines.reserve(segments * 12 + 16);
        const auto appendLine = [&lines](FXMVECTOR a, FXMVECTOR b)
        {
            XMFLOAT3 first{};
            XMFLOAT3 second{};
            XMStoreFloat3(&first, a);
            XMStoreFloat3(&second, b);
            lines.push_back(first);
            lines.push_back(second);
        };
        const auto radial = [&](const float angle)
        {
            return XMVectorScale(
                XMVectorAdd(
                    XMVectorScale(tangent, std::cos(angle)),
                    XMVectorScale(bitangent, std::sin(angle))),
                capsule.radius);
        };
        for (int index{}; index < segments; ++index)
        {
            const float a =
                XM_2PI * static_cast<float>(index)
                / static_cast<float>(segments);
            const float b =
                XM_2PI * static_cast<float>(index + 1)
                / static_cast<float>(segments);
            appendLine(
                XMVectorAdd(start, radial(a)),
                XMVectorAdd(start, radial(b)));
            appendLine(
                XMVectorAdd(end, radial(a)),
                XMVectorAdd(end, radial(b)));
        }
        for (int index{}; index < 4; ++index)
        {
            const auto direction = radial(
                XM_PIDIV2 * static_cast<float>(index));
            appendLine(
                XMVectorAdd(start, direction),
                XMVectorAdd(end, direction));
        }
        for (const XMVECTOR plane : { tangent, bitangent })
        {
            for (int index{}; index < segments / 2; ++index)
            {
                const float a =
                    XM_PI * static_cast<float>(index)
                    / static_cast<float>(segments / 2);
                const float b =
                    XM_PI * static_cast<float>(index + 1)
                    / static_cast<float>(segments / 2);
                const auto capPoint =
                    [&](FXMVECTOR center, const float angle, const float sign)
                    {
                        return XMVectorAdd(
                            center,
                            XMVectorScale(
                                XMVectorAdd(
                                    XMVectorScale(plane, std::cos(angle)),
                                    XMVectorScale(axis, sign * std::sin(angle))),
                                capsule.radius));
                    };
                appendLine(
                    capPoint(start, a, -1.0f),
                    capPoint(start, b, -1.0f));
                appendLine(
                    capPoint(end, a, 1.0f),
                    capPoint(end, b, 1.0f));
            }
        }
        graphics.Debug().DrawLines(lines, color, view, projection);
    }
}
