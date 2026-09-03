#pragma once

#include "LamaPon/Scene/Component.h"

#include <DirectXMath.h>

#include <cstdint>

namespace LamaPon
{
    enum class JointType
    {
        Fixed,
        Hinge,
        Spring
    };

    struct HingeLimits final
    {
        float minimumAngleDegrees{ -90.0f };
        float maximumAngleDegrees{ 90.0f };
    };

    struct HingeMotor final
    {
        float targetVelocityDegrees{ 90.0f };
        float maximumTorque{ 10.0f };
    };

    class JointComponent final : public Component
    {
    public:
        explicit JointComponent(
            JointType type = JointType::Fixed,
            std::uint64_t connectedBodyId = 0,
            DirectX::XMFLOAT3 anchor = {},
            DirectX::XMFLOAT3 connectedAnchor = {},
            DirectX::XMFLOAT3 axis = { 0.0f, 1.0f, 0.0f },
            float restLength = 1.0f,
            float stiffness = 20.0f,
            float damping = 2.0f,
            bool collideConnected = false,
            bool useLimits = false,
            HingeLimits limits = {},
            bool useMotor = false,
            HingeMotor motor = {}) noexcept;

        [[nodiscard]] JointType Type() const noexcept { return m_type; }
        void SetType(JointType value) noexcept;

        [[nodiscard]] std::uint64_t ConnectedBodyId() const noexcept
        {
            return m_connectedBodyId;
        }
        void SetConnectedBodyId(std::uint64_t value) noexcept;

        [[nodiscard]] const DirectX::XMFLOAT3& Anchor() const noexcept
        {
            return m_anchor;
        }
        void SetAnchor(const DirectX::XMFLOAT3& value) noexcept
        {
            m_anchor = value;
        }

        [[nodiscard]] const DirectX::XMFLOAT3& ConnectedAnchor() const noexcept
        {
            return m_connectedAnchor;
        }
        void SetConnectedAnchor(const DirectX::XMFLOAT3& value) noexcept
        {
            m_connectedAnchor = value;
        }

        [[nodiscard]] const DirectX::XMFLOAT3& Axis() const noexcept
        {
            return m_axis;
        }
        void SetAxis(const DirectX::XMFLOAT3& value) noexcept;

        [[nodiscard]] float RestLength() const noexcept { return m_restLength; }
        void SetRestLength(float value) noexcept;
        [[nodiscard]] float Stiffness() const noexcept { return m_stiffness; }
        void SetStiffness(float value) noexcept;
        [[nodiscard]] float Damping() const noexcept { return m_damping; }
        void SetDamping(float value) noexcept;

        [[nodiscard]] bool UseLimits() const noexcept
        {
            return m_useLimits;
        }
        void SetUseLimits(bool value) noexcept
        {
            m_useLimits = value;
        }
        [[nodiscard]] const HingeLimits& Limits() const noexcept
        {
            return m_limits;
        }
        void SetLimits(HingeLimits value) noexcept;

        [[nodiscard]] bool UseMotor() const noexcept
        {
            return m_useMotor;
        }
        void SetUseMotor(bool value) noexcept
        {
            m_useMotor = value;
        }
        [[nodiscard]] const HingeMotor& Motor() const noexcept
        {
            return m_motor;
        }
        void SetMotor(HingeMotor value) noexcept;

        void EnsureHingeReference(
            const GameObject& connected) noexcept;
        void ResetHingeReference() noexcept;
        [[nodiscard]] DirectX::XMFLOAT3 HingeWorldAxis(
            const GameObject& connected) const noexcept;
        [[nodiscard]] float HingeAngleRadians(
            const GameObject& connected) const noexcept;

        [[nodiscard]] bool CollideConnected() const noexcept
        {
            return m_collideConnected;
        }
        void SetCollideConnected(bool value) noexcept
        {
            m_collideConnected = value;
        }

        [[nodiscard]] std::string_view TypeName() const noexcept override
        {
            return "Joint";
        }

    private:
        JointType m_type{ JointType::Fixed };
        std::uint64_t m_connectedBodyId{};
        DirectX::XMFLOAT3 m_anchor{};
        DirectX::XMFLOAT3 m_connectedAnchor{};
        DirectX::XMFLOAT3 m_axis{ 0.0f, 1.0f, 0.0f };
        float m_restLength{ 1.0f };
        float m_stiffness{ 20.0f };
        float m_damping{ 2.0f };
        bool m_useLimits{};
        HingeLimits m_limits;
        bool m_useMotor{};
        HingeMotor m_motor;
        bool m_collideConnected{};
        bool m_hingeReferenceInitialized{};
        DirectX::XMFLOAT3 m_hingeOwnerReference{
            1.0f,
            0.0f,
            0.0f
        };
        DirectX::XMFLOAT3 m_hingeConnectedAxis{
            0.0f,
            1.0f,
            0.0f
        };
        DirectX::XMFLOAT3 m_hingeConnectedReference{
            1.0f,
            0.0f,
            0.0f
        };
    };
}
