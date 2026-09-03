#pragma once

#include "LamaPon/Physics/Collision3D.h"
#include "LamaPon/Physics/PhysicsMaterial.h"
#include "LamaPon/Scene/Component.h"

#include <DirectXMath.h>

#include <cstdint>

namespace LamaPon
{
    class SphereCollider3DComponent final : public Component
    {
    public:
        explicit SphereCollider3DComponent(
            float radius = 0.5f,
            DirectX::XMFLOAT3 offset = {},
            bool isTrigger = false,
            std::uint32_t layer = 0,
            std::uint32_t collisionMask = 0xffffffffu,
            PhysicsMaterial material = {}) noexcept;

        [[nodiscard]] float Radius() const noexcept
        {
            return m_radius;
        }
        void SetRadius(float value) noexcept;
        [[nodiscard]] const DirectX::XMFLOAT3&
            Offset() const noexcept
        {
            return m_offset;
        }
        void SetOffset(
            const DirectX::XMFLOAT3& value) noexcept
        {
            m_offset = value;
        }
        [[nodiscard]] bool IsTrigger() const noexcept
        {
            return m_isTrigger;
        }
        void SetTrigger(bool value) noexcept
        {
            m_isTrigger = value;
        }
        [[nodiscard]] std::uint32_t Layer() const noexcept
        {
            return m_layer;
        }
        void SetLayer(std::uint32_t value) noexcept
        {
            m_layer = value % 32u;
        }
        [[nodiscard]] std::uint32_t
            CollisionMask() const noexcept
        {
            return m_collisionMask;
        }
        void SetCollisionMask(
            std::uint32_t value) noexcept
        {
            m_collisionMask = value;
        }
        [[nodiscard]] const PhysicsMaterial&
            Material() const noexcept
        {
            return m_material;
        }
        void SetMaterial(PhysicsMaterial value) noexcept
        {
            value.Clamp();
            m_material = value;
        }

        [[nodiscard]] Sphere3D WorldSphere() const noexcept;
        [[nodiscard]] Bounds3D WorldBounds() const noexcept;
        [[nodiscard]] std::string_view
            TypeName() const noexcept override
        {
            return "SphereCollider3D";
        }

    protected:
        void OnRenderDebug3D(
            GraphicsDevice& graphics,
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX projection) override;

    private:
        float m_radius{ 0.5f };
        DirectX::XMFLOAT3 m_offset{};
        bool m_isTrigger{};
        std::uint32_t m_layer{};
        std::uint32_t m_collisionMask{
            0xffffffffu
        };
        PhysicsMaterial m_material;
    };
}
