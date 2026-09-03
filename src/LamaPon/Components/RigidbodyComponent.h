#pragma once

#include "LamaPon/Scene/Component.h"

#include <DirectXMath.h>

namespace LamaPon
{
    enum class CollisionDetectionMode
    {
        Discrete,
        Continuous
    };

    enum class ForceMode
    {
        Force,
        Acceleration,
        Impulse,
        VelocityChange
    };

    struct RigidbodyConstraints final
    {
        bool freezeRotationX{};
        bool freezeRotationY{};
        bool freezeRotationZ{};
        // 位置の凍結。freezePositionZ＋freezeRotationX/Yで
        // 2D（XY平面）のRigidbodyとして使えます。
        bool freezePositionX{};
        bool freezePositionY{};
        bool freezePositionZ{};
    };

    class RigidbodyComponent final : public Component
    {
    public:
        explicit RigidbodyComponent(
            DirectX::XMFLOAT3 velocity = { 0.0f, 0.0f, 0.0f },
            bool useGravity = true,
            bool isKinematic = false,
            CollisionDetectionMode collisionDetection =
                CollisionDetectionMode::Discrete,
            float mass = 1.0f,
            DirectX::XMFLOAT3 angularVelocity = {},
            DirectX::XMFLOAT3 centerOfMass = {},
            float linearDrag = 0.0f,
            float angularDrag = 0.05f,
            RigidbodyConstraints constraints = {},
            bool interpolate = true) noexcept;

        [[nodiscard]] const DirectX::XMFLOAT3& Velocity() const noexcept
        {
            return m_velocity;
        }
        void SetVelocity(
            const DirectX::XMFLOAT3& velocity) noexcept;
        [[nodiscard]] const DirectX::XMFLOAT3&
            AngularVelocity() const noexcept
        {
            return m_angularVelocity;
        }
        void SetAngularVelocity(
            DirectX::XMFLOAT3 value) noexcept;
        [[nodiscard]] float Mass() const noexcept { return m_mass; }
        void SetMass(float value) noexcept;
        [[nodiscard]] float InverseMass() const noexcept
        {
            return !m_isKinematic
                ? 1.0f / m_mass
                : 0.0f;
        }
        [[nodiscard]] const DirectX::XMFLOAT3&
            CenterOfMass() const noexcept
        {
            return m_centerOfMass;
        }
        void SetCenterOfMass(
            const DirectX::XMFLOAT3& value) noexcept
        {
            m_centerOfMass = value;
        }
        [[nodiscard]] float LinearDrag() const noexcept
        {
            return m_linearDrag;
        }
        void SetLinearDrag(float value) noexcept;
        [[nodiscard]] float AngularDrag() const noexcept
        {
            return m_angularDrag;
        }
        void SetAngularDrag(float value) noexcept;
        [[nodiscard]] const RigidbodyConstraints&
            Constraints() const noexcept
        {
            return m_constraints;
        }
        void SetConstraints(
            RigidbodyConstraints value) noexcept;

        [[nodiscard]] bool UsesGravity() const noexcept { return m_useGravity; }
        void SetUseGravity(const bool useGravity) noexcept { m_useGravity = useGravity; }

        [[nodiscard]] bool IsKinematic() const noexcept { return m_isKinematic; }
        void SetKinematic(bool kinematic) noexcept;

        [[nodiscard]] CollisionDetectionMode CollisionDetection() const noexcept
        {
            return m_collisionDetection;
        }
        void SetCollisionDetection(
            const CollisionDetectionMode value) noexcept
        {
            m_collisionDetection = value;
            // 判定方式を変えたら警告をもう一度出せるようにします
            // （CCDへ移して直したのに黙ったまま、を避けるため）。
            m_discreteSpeedWarned = false;
        }
        [[nodiscard]] bool Interpolates() const noexcept
        {
            return m_interpolate;
        }
        void SetInterpolate(
            const bool value) noexcept
        {
            m_interpolate = value;
        }
        [[nodiscard]] bool UsesContinuousCollisionDetection() const noexcept
        {
            return m_collisionDetection == CollisionDetectionMode::Continuous;
        }

        void Integrate(GameObject& gameObject, float deltaTime) noexcept;
        void ClearAccumulators() noexcept;
        [[nodiscard]] bool IsSleeping() const noexcept
        {
            return m_isSleeping;
        }
        void Sleep() noexcept;
        void WakeUp() noexcept;
        void UpdateSleepState(
            float deltaTime,
            bool supported) noexcept;
        void AddForce(
            const DirectX::XMFLOAT3& force,
            ForceMode mode = ForceMode::Force) noexcept;
        void AddTorque(
            const DirectX::XMFLOAT3& torque,
            ForceMode mode = ForceMode::Force) noexcept;
        void AddForceAtPosition(
            const DirectX::XMFLOAT3& force,
            const DirectX::XMFLOAT3& worldPosition,
            ForceMode mode = ForceMode::Force) noexcept;
        [[nodiscard]] DirectX::XMFLOAT3
            WorldCenterOfMass() const noexcept;
        [[nodiscard]] DirectX::XMFLOAT3
            VelocityAtPoint(
                const DirectX::XMFLOAT3& worldPosition) const noexcept;
        [[nodiscard]] DirectX::XMFLOAT3
            ApplyInverseInertia(
                const DirectX::XMFLOAT3& worldTorque) const noexcept;
        void ApplyImpulseAtPoint(
            const DirectX::XMFLOAT3& impulse,
            const DirectX::XMFLOAT3& worldPosition) noexcept;
        void RemoveInwardVelocity(const DirectX::XMFLOAT3& surfaceNormal) noexcept;

        [[nodiscard]] std::string_view TypeName() const noexcept override
        {
            return "Rigidbody";
        }

    private:
        void SetAngularVelocityInternal(
            DirectX::XMFLOAT3 value,
            bool wake) noexcept;

        DirectX::XMFLOAT3 m_velocity;
        DirectX::XMFLOAT3 m_angularVelocity{};
        DirectX::XMFLOAT3 m_centerOfMass{};
        DirectX::XMFLOAT3 m_accumulatedForce{};
        DirectX::XMFLOAT3 m_accumulatedTorque{};
        DirectX::XMFLOAT3 m_accumulatedAcceleration{};
        DirectX::XMFLOAT3 m_accumulatedAngularAcceleration{};
        float m_mass{ 1.0f };
        float m_linearDrag{};
        float m_angularDrag{ 0.05f };
        RigidbodyConstraints m_constraints;
        bool m_useGravity{ true };
        bool m_isKinematic{};
        bool m_isSleeping{};
        // 「DCDには速すぎる」警告を出したか。毎フレーム出すと
        // ログが埋まって他が読めなくなるので、1体につき1回だけです。
        bool m_discreteSpeedWarned{};
        bool m_interpolate{ true };
        float m_sleepTimer{};
        CollisionDetectionMode m_collisionDetection{
            CollisionDetectionMode::Discrete
        };
    };
}
