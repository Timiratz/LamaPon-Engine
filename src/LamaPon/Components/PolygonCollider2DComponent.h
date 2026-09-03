#pragma once

#include "LamaPon/Physics/CollisionTypes.h"
#include "LamaPon/Physics/PhysicsMaterial.h"
#include "LamaPon/Scene/Component.h"

#include <DirectXMath.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace LamaPon
{
    // ワールド空間の凸多角形（XY平面）。頂点は3個以上を想定します。
    struct Polygon2D final
    {
        std::vector<DirectX::XMFLOAT2> vertices;
    };

    class PolygonCollider2DComponent final : public Component
    {
    public:
        explicit PolygonCollider2DComponent(
            std::vector<DirectX::XMFLOAT2> vertices = DefaultVertices(),
            DirectX::XMFLOAT2 offset = { 0.0f, 0.0f },
            bool isTrigger = false,
            std::uint32_t layer = 0,
            std::uint32_t collisionMask = 0xffffffffu,
            PhysicsMaterial material = {}) noexcept;

        [[nodiscard]] const std::vector<DirectX::XMFLOAT2>&
            Vertices() const noexcept
        {
            return m_vertices;
        }
        void SetVertices(std::vector<DirectX::XMFLOAT2> vertices) noexcept;
        void SetVertex(
            std::size_t index,
            const DirectX::XMFLOAT2& value) noexcept;
        void AddVertex(
            const DirectX::XMFLOAT2& value = { 0.0f, 0.0f }) noexcept;
        void RemoveVertex(std::size_t index) noexcept;

        [[nodiscard]] const DirectX::XMFLOAT2& Offset() const noexcept
        {
            return m_offset;
        }
        void SetOffset(const DirectX::XMFLOAT2& offset) noexcept
        {
            m_offset = offset;
        }
        [[nodiscard]] bool IsTrigger() const noexcept { return m_isTrigger; }
        void SetTrigger(const bool trigger) noexcept { m_isTrigger = trigger; }
        [[nodiscard]] std::uint32_t Layer() const noexcept { return m_layer; }
        void SetLayer(const std::uint32_t layer) noexcept { m_layer = layer % 32u; }
        [[nodiscard]] std::uint32_t CollisionMask() const noexcept
        {
            return m_collisionMask;
        }
        void SetCollisionMask(const std::uint32_t mask) noexcept
        {
            m_collisionMask = mask;
        }
        [[nodiscard]] const PhysicsMaterial& Material() const noexcept
        {
            return m_material;
        }
        void SetMaterial(PhysicsMaterial value) noexcept
        {
            value.Clamp();
            m_material = value;
        }

        [[nodiscard]] Polygon2D WorldPolygon() const noexcept;
        [[nodiscard]] Bounds2D WorldBounds() const noexcept;
        [[nodiscard]] std::string_view TypeName() const noexcept override
        {
            return "PolygonCollider2D";
        }

    protected:
        void OnRenderDebug3D(
            GraphicsDevice& graphics,
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX projection) override;

    private:
        [[nodiscard]] static std::vector<DirectX::XMFLOAT2> DefaultVertices();

        std::vector<DirectX::XMFLOAT2> m_vertices;
        DirectX::XMFLOAT2 m_offset;
        bool m_isTrigger{};
        std::uint32_t m_layer{};
        std::uint32_t m_collisionMask{ 0xffffffffu };
        PhysicsMaterial m_material;
    };
}
