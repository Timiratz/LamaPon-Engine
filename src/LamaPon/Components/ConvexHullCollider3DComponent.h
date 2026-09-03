#pragma once

#include "LamaPon/Physics/Collision3D.h"
#include "LamaPon/Physics/PhysicsMaterial.h"
#include "LamaPon/Scene/Component.h"

#include <DirectXMath.h>

#include <cstdint>
#include <vector>

namespace LamaPon
{
    class ConvexHullCollider3DComponent final : public Component
    {
    public:
        explicit ConvexHullCollider3DComponent(
            std::vector<DirectX::XMFLOAT3> points = DefaultPoints(),
            DirectX::XMFLOAT3 offset = {},
            bool isTrigger = false,
            std::uint32_t layer = 0,
            std::uint32_t collisionMask = 0xffffffffu,
            PhysicsMaterial material = {}) noexcept;

        [[nodiscard]] const std::vector<DirectX::XMFLOAT3>&
            Points() const noexcept
        {
            return m_points;
        }
        void SetPoints(std::vector<DirectX::XMFLOAT3> points) noexcept;
        void SetPoint(
            std::size_t index,
            const DirectX::XMFLOAT3& value) noexcept;
        void AddPoint(
            const DirectX::XMFLOAT3& value =
                { 0.0f, 0.0f, 0.0f }) noexcept;
        void RemovePoint(std::size_t index) noexcept;

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
        void SetCollisionMask(std::uint32_t value) noexcept
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

        [[nodiscard]] ConvexHull3D WorldHull() const noexcept;
        [[nodiscard]] Bounds3D WorldBounds() const noexcept;
        [[nodiscard]] std::string_view
            TypeName() const noexcept override
        {
            return "ConvexHullCollider3D";
        }

    protected:
        void OnRenderDebug3D(
            GraphicsDevice& graphics,
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX projection) override;

    private:
        [[nodiscard]] static std::vector<DirectX::XMFLOAT3>
            DefaultPoints();

        std::vector<DirectX::XMFLOAT3> m_points;
        DirectX::XMFLOAT3 m_offset{};
        bool m_isTrigger{};
        std::uint32_t m_layer{};
        std::uint32_t m_collisionMask{ 0xffffffffu };
        PhysicsMaterial m_material;
    };
}
