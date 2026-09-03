#pragma once

#include "LamaPon/Physics/CollisionTypes.h"
#include "LamaPon/Physics/PhysicsMaterial.h"
#include "LamaPon/Scene/Component.h"

#include <DirectXMath.h>
#include <cstdint>

namespace LamaPon
{
    class BoxCollider2DComponent final : public Component
    {
    public:
        explicit BoxCollider2DComponent(
            DirectX::XMFLOAT2 size = { 1.0f, 1.0f },
            DirectX::XMFLOAT2 offset = { 0.0f, 0.0f },
            bool isTrigger = false,
            std::uint32_t layer = 0,
            std::uint32_t collisionMask = 0xffffffffu,
            PhysicsMaterial material = {}) noexcept;

        [[nodiscard]] const DirectX::XMFLOAT2& Size() const noexcept { return m_size; }
        void SetSize(const DirectX::XMFLOAT2& size) noexcept { m_size = size; }

        [[nodiscard]] const DirectX::XMFLOAT2& Offset() const noexcept { return m_offset; }
        void SetOffset(const DirectX::XMFLOAT2& offset) noexcept { m_offset = offset; }

        [[nodiscard]] bool IsTrigger() const noexcept { return m_isTrigger; }
        void SetTrigger(const bool trigger) noexcept { m_isTrigger = trigger; }
        [[nodiscard]] std::uint32_t Layer() const noexcept { return m_layer; }
        void SetLayer(const std::uint32_t layer) noexcept { m_layer = layer % 32u; }
        [[nodiscard]] std::uint32_t CollisionMask() const noexcept { return m_collisionMask; }
        void SetCollisionMask(const std::uint32_t mask) noexcept { m_collisionMask = mask; }
        [[nodiscard]] const PhysicsMaterial& Material() const noexcept
        {
            return m_material;
        }
        void SetMaterial(PhysicsMaterial value) noexcept
        {
            value.Clamp();
            m_material = value;
        }
        [[nodiscard]] bool CanCollideWith(
            const BoxCollider2DComponent& other) const noexcept;

        [[nodiscard]] Bounds2D WorldBounds() const noexcept;
        [[nodiscard]] std::string_view TypeName() const noexcept override
        {
            return "BoxCollider2D";
        }

    protected:
        void OnRenderDebug3D(
            GraphicsDevice& graphics,
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX projection) override;

    private:
        DirectX::XMFLOAT2 m_size;
        DirectX::XMFLOAT2 m_offset;
        bool m_isTrigger{};
        std::uint32_t m_layer{};
        std::uint32_t m_collisionMask{ 0xffffffffu };
        PhysicsMaterial m_material;
    };
}
