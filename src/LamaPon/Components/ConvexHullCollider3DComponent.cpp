#include "LamaPon/Components/ConvexHullCollider3DComponent.h"

#include "LamaPon/Graphics/DebugRenderer.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Scene/GameObject.h"

#include <array>
#include <cstddef>
#include <utility>
#include <vector>

namespace LamaPon
{
    std::vector<DirectX::XMFLOAT3>
        ConvexHullCollider3DComponent::DefaultPoints()
    {
        return {
            { 0.5f, 0.0f, 0.0f },
            { -0.5f, 0.0f, 0.0f },
            { 0.0f, 0.5f, 0.0f },
            { 0.0f, -0.5f, 0.0f },
            { 0.0f, 0.0f, 0.5f },
            { 0.0f, 0.0f, -0.5f }
        };
    }

    ConvexHullCollider3DComponent::
        ConvexHullCollider3DComponent(
            std::vector<DirectX::XMFLOAT3> points,
            const DirectX::XMFLOAT3 offset,
            const bool isTrigger,
            const std::uint32_t layer,
            const std::uint32_t collisionMask,
            PhysicsMaterial material) noexcept
        : m_points(
            points.empty()
                ? DefaultPoints()
                : std::move(points))
        , m_offset(offset)
        , m_isTrigger(isTrigger)
        , m_layer(layer % 32u)
        , m_collisionMask(collisionMask)
        , m_material(material)
    {
        m_material.Clamp();
    }

    void ConvexHullCollider3DComponent::SetPoints(
        std::vector<DirectX::XMFLOAT3> points) noexcept
    {
        m_points = std::move(points);
    }

    void ConvexHullCollider3DComponent::SetPoint(
        const std::size_t index,
        const DirectX::XMFLOAT3& value) noexcept
    {
        if (index < m_points.size())
        {
            m_points[index] = value;
        }
    }

    void ConvexHullCollider3DComponent::AddPoint(
        const DirectX::XMFLOAT3& value) noexcept
    {
        m_points.push_back(value);
    }

    void ConvexHullCollider3DComponent::RemovePoint(
        const std::size_t index) noexcept
    {
        if (index < m_points.size())
        {
            m_points.erase(
                m_points.begin()
                    + static_cast<std::ptrdiff_t>(index));
        }
    }

    ConvexHull3D
        ConvexHullCollider3DComponent::WorldHull()
            const noexcept
    {
        using namespace DirectX;
        const XMMATRIX world = Owner().WorldMatrix();
        ConvexHull3D result;
        result.points.reserve(m_points.size());
        for (const auto& point : m_points)
        {
            const XMVECTOR local = XMVectorAdd(
                XMLoadFloat3(&point),
                XMLoadFloat3(&m_offset));
            XMFLOAT3 worldPoint{};
            XMStoreFloat3(
                &worldPoint,
                XMVector3TransformCoord(local, world));
            result.points.push_back(worldPoint);
        }
        return result;
    }

    Bounds3D
        ConvexHullCollider3DComponent::WorldBounds()
            const noexcept
    {
        return BoundsOf(WorldHull());
    }

    void ConvexHullCollider3DComponent::OnRenderDebug3D(
        GraphicsDevice& graphics,
        DirectX::FXMMATRIX view,
        DirectX::CXMMATRIX projection)
    {
        using namespace DirectX;
        const auto hull = WorldHull();
        const auto bounds = BoundsOf(hull);
        const XMVECTOR color = m_isTrigger
            ? XMVectorSet(1.0f, 0.75f, 0.1f, 1.0f)
            : XMVectorSet(0.5f, 0.65f, 1.0f, 1.0f);

        std::vector<XMFLOAT3> lines;

        std::array<XMFLOAT3, 8> corners;
        std::size_t cornerIndex{};
        for (int x = -1; x <= 1; x += 2)
        {
            for (int y = -1; y <= 1; y += 2)
            {
                for (int z = -1; z <= 1; z += 2)
                {
                    corners[cornerIndex++] = {
                        x < 0 ? bounds.minimum.x : bounds.maximum.x,
                        y < 0 ? bounds.minimum.y : bounds.maximum.y,
                        z < 0 ? bounds.minimum.z : bounds.maximum.z
                    };
                }
            }
        }
        for (std::size_t corner{}; corner < corners.size(); ++corner)
        {
            for (const std::size_t bit : { 1u, 2u, 4u })
            {
                const std::size_t neighbor = corner ^ bit;
                if (corner < neighbor)
                {
                    lines.push_back(corners[corner]);
                    lines.push_back(corners[neighbor]);
                }
            }
        }

        constexpr float markerSize = 0.06f;
        for (const auto& point : hull.points)
        {
            lines.push_back(
                { point.x - markerSize, point.y, point.z });
            lines.push_back(
                { point.x + markerSize, point.y, point.z });
            lines.push_back(
                { point.x, point.y - markerSize, point.z });
            lines.push_back(
                { point.x, point.y + markerSize, point.z });
            lines.push_back(
                { point.x, point.y, point.z - markerSize });
            lines.push_back(
                { point.x, point.y, point.z + markerSize });
        }

        graphics.Debug().DrawLines(lines, color, view, projection);
    }
}
