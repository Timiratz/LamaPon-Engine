#pragma once

#include "LamaPon/Scene/Component.h"

#include <DirectXMath.h>

namespace LamaPon
{
    class RotatorComponent final : public Component
    {
    public:
        explicit RotatorComponent(
            DirectX::XMFLOAT3 angularVelocity = { 0.0f, 1.0f, 0.0f }) noexcept;

        [[nodiscard]] const DirectX::XMFLOAT3& AngularVelocity() const noexcept
        {
            return m_angularVelocity;
        }
        void SetAngularVelocity(const DirectX::XMFLOAT3& velocity) noexcept
        {
            m_angularVelocity = velocity;
        }

        [[nodiscard]] std::string_view TypeName() const noexcept override { return "Rotator"; }

    protected:
        void OnUpdate(float deltaTime) override;

    private:
        DirectX::XMFLOAT3 m_angularVelocity;
    };
}
