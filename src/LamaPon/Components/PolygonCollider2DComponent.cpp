#include "LamaPon/Components/PolygonCollider2DComponent.h"

#include "LamaPon/Graphics/DebugRenderer.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Scene/GameObject.h"

#include <limits>
#include <utility>

namespace LamaPon
{
    std::vector<DirectX::XMFLOAT2>
        PolygonCollider2DComponent::DefaultVertices()
    {
        return {
            { 0.0f, 0.5f },
            { -0.5f, -0.5f },
            { 0.5f, -0.5f }
        };
    }

    PolygonCollider2DComponent::PolygonCollider2DComponent(
        std::vector<DirectX::XMFLOAT2> vertices,
        const DirectX::XMFLOAT2 offset,
        const bool isTrigger,
        const std::uint32_t layer,
        const std::uint32_t collisionMask,
        PhysicsMaterial material) noexcept
        : m_vertices(
            vertices.empty() ? DefaultVertices() : std::move(vertices))
        , m_offset(offset)
        , m_isTrigger(isTrigger)
        , m_layer(layer % 32u)
        , m_collisionMask(collisionMask)
        , m_material(material)
    {
        m_material.Clamp();
    }

    void PolygonCollider2DComponent::SetVertices(
        std::vector<DirectX::XMFLOAT2> vertices) noexcept
    {
        m_vertices = std::move(vertices);
    }

    void PolygonCollider2DComponent::SetVertex(
        const std::size_t index,
        const DirectX::XMFLOAT2& value) noexcept
    {
        if (index < m_vertices.size())
        {
            m_vertices[index] = value;
        }
    }

    void PolygonCollider2DComponent::AddVertex(
        const DirectX::XMFLOAT2& value) noexcept
    {
        m_vertices.push_back(value);
    }

    void PolygonCollider2DComponent::RemoveVertex(
        const std::size_t index) noexcept
    {
        if (index < m_vertices.size())
        {
            m_vertices.erase(
                m_vertices.begin() + static_cast<std::ptrdiff_t>(index));
        }
    }

    Polygon2D PolygonCollider2DComponent::WorldPolygon() const noexcept
    {
        using namespace DirectX;

        const XMMATRIX world = Owner().WorldMatrix();
        Polygon2D result;
        result.vertices.reserve(m_vertices.size());
        for (const auto& vertex : m_vertices)
        {
            const XMFLOAT3 local{
                vertex.x + m_offset.x,
                vertex.y + m_offset.y,
                0.0f
            };
            XMFLOAT3 worldVertex{};
            XMStoreFloat3(
                &worldVertex,
                XMVector3TransformCoord(XMLoadFloat3(&local), world));
            result.vertices.push_back({ worldVertex.x, worldVertex.y });
        }
        return result;
    }

    Bounds2D PolygonCollider2DComponent::WorldBounds() const noexcept
    {
        Bounds2D bounds{
            { std::numeric_limits<float>::max(), std::numeric_limits<float>::max() },
            { std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest() }
        };

        const auto polygon = WorldPolygon();
        for (const auto& vertex : polygon.vertices)
        {
            bounds.minimum.x = std::min(bounds.minimum.x, vertex.x);
            bounds.minimum.y = std::min(bounds.minimum.y, vertex.y);
            bounds.maximum.x = std::max(bounds.maximum.x, vertex.x);
            bounds.maximum.y = std::max(bounds.maximum.y, vertex.y);
        }
        if (polygon.vertices.empty())
        {
            bounds = { { 0.0f, 0.0f }, { 0.0f, 0.0f } };
        }
        return bounds;
    }

    void PolygonCollider2DComponent::OnRenderDebug3D(
        GraphicsDevice& graphics,
        DirectX::FXMMATRIX view,
        DirectX::CXMMATRIX projection)
    {
        const auto polygon = WorldPolygon();
        if (polygon.vertices.size() < 2)
        {
            return;
        }

        std::vector<DirectX::XMFLOAT3> lines;
        lines.reserve(polygon.vertices.size() * 2);
        for (std::size_t index{}; index < polygon.vertices.size(); ++index)
        {
            const auto& a = polygon.vertices[index];
            const auto& b =
                polygon.vertices[(index + 1) % polygon.vertices.size()];
            lines.push_back({ a.x, a.y, 0.0f });
            lines.push_back({ b.x, b.y, 0.0f });
        }

        const DirectX::XMVECTOR color = m_isTrigger
            ? DirectX::XMVectorSet(1.0f, 0.75f, 0.1f, 1.0f)
            : DirectX::XMVectorSet(0.1f, 1.0f, 0.35f, 1.0f);
        graphics.Debug().DrawLines(lines, color, view, projection);
    }
}
