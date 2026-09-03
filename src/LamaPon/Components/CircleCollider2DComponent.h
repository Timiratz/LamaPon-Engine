#pragma once

#include "LamaPon/Physics/PhysicsMaterial.h"
#include "LamaPon/Scene/Component.h"

#include <DirectXMath.h>

#include <cstdint>

namespace LamaPon
{
    struct Bounds2D;
    class GraphicsDevice;

    // ワールド空間の2D円（XY平面）。
    struct Circle2D final
    {
        DirectX::XMFLOAT2 center{};
        float radius{ 0.5f };
    };

    class CircleCollider2DComponent final : public Component
    {
    public:
        explicit CircleCollider2DComponent(
            float radius = 0.5f,
            DirectX::XMFLOAT2 offset = { 0.0f, 0.0f },
            bool isTrigger = false,
            std::uint32_t layer = 0,
            std::uint32_t collisionMask = 0xffffffffu,
            PhysicsMaterial material = {}) noexcept;

        [[nodiscard]] float Radius() const noexcept
        {
            return m_radius;
        }
        void SetRadius(float radius) noexcept;
        [[nodiscard]] const DirectX::XMFLOAT2&
            Offset() const noexcept
        {
            return m_offset;
        }
        void SetOffset(
            const DirectX::XMFLOAT2& offset) noexcept
        {
            m_offset = offset;
        }
        [[nodiscard]] bool IsTrigger() const noexcept
        {
            return m_isTrigger;
        }
        void SetTrigger(const bool trigger) noexcept
        {
            m_isTrigger = trigger;
        }
        [[nodiscard]] std::uint32_t Layer() const noexcept
        {
            return m_layer;
        }
        void SetLayer(const std::uint32_t layer) noexcept
        {
            m_layer = layer % 32u;
        }
        [[nodiscard]] std::uint32_t
            CollisionMask() const noexcept
        {
            return m_collisionMask;
        }
        void SetCollisionMask(
            const std::uint32_t mask) noexcept
        {
            m_collisionMask = mask;
        }
        [[nodiscard]] const PhysicsMaterial&
            Material() const noexcept
        {
            return m_material;
        }
        void SetMaterial(
            const PhysicsMaterial& material) noexcept
        {
            m_material = material;
            m_material.Clamp();
        }

        [[nodiscard]] Circle2D WorldCircle() const noexcept;
        [[nodiscard]] Bounds2D WorldBounds() const noexcept;

        [[nodiscard]] std::string_view
            TypeName() const noexcept override
        {
            return "CircleCollider2D";
        }

    protected:
        void OnRenderDebug3D(
            GraphicsDevice& graphics,
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX projection) override;

    private:
        float m_radius;
        DirectX::XMFLOAT2 m_offset;
        bool m_isTrigger;
        std::uint32_t m_layer;
        std::uint32_t m_collisionMask;
        PhysicsMaterial m_material;
    };
}
