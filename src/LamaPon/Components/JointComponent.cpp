#include "LamaPon/Components/JointComponent.h"

#include "LamaPon/Scene/GameObject.h"

#include <algorithm>
#include <cmath>

namespace
{
    DirectX::XMFLOAT3 Normalize(
        const DirectX::XMFLOAT3& value,
        const DirectX::XMFLOAT3& fallback = {
            0.0f,
            1.0f,
            0.0f }) noexcept
    {
        const float length = std::sqrt(
            value.x * value.x
            + value.y * value.y
            + value.z * value.z);
        return length > 0.000001f
            ? DirectX::XMFLOAT3{
                value.x / length,
                value.y / length,
                value.z / length
            }
            : fallback;
    }

    DirectX::XMFLOAT3 Cross(
        const DirectX::XMFLOAT3& left,
        const DirectX::XMFLOAT3& right) noexcept
    {
        return {
            left.y * right.z - left.z * right.y,
            left.z * right.x - left.x * right.z,
            left.x * right.y - left.y * right.x
        };
    }

    float Dot(
        const DirectX::XMFLOAT3& left,
        const DirectX::XMFLOAT3& right) noexcept
    {
        return left.x * right.x
            + left.y * right.y
            + left.z * right.z;
    }

    DirectX::XMFLOAT3 TransformDirection(
        const DirectX::XMFLOAT3& value,
        DirectX::FXMMATRIX matrix) noexcept
    {
        DirectX::XMFLOAT3 result{};
        DirectX::XMStoreFloat3(
            &result,
            DirectX::XMVector3TransformNormal(
                DirectX::XMLoadFloat3(&value),
                matrix));
        return Normalize(result);
    }

    DirectX::XMFLOAT3 ProjectToPlane(
        const DirectX::XMFLOAT3& value,
        const DirectX::XMFLOAT3& normal) noexcept
    {
        const float projection = Dot(value, normal);
        return Normalize({
            value.x - normal.x * projection,
            value.y - normal.y * projection,
            value.z - normal.z * projection
        });
    }
}

namespace LamaPon
{
    JointComponent::JointComponent(
        const JointType type,
        const std::uint64_t connectedBodyId,
        const DirectX::XMFLOAT3 anchor,
        const DirectX::XMFLOAT3 connectedAnchor,
        const DirectX::XMFLOAT3 axis,
        const float restLength,
        const float stiffness,
        const float damping,
        const bool collideConnected,
        const bool useLimits,
        const HingeLimits limits,
        const bool useMotor,
        const HingeMotor motor) noexcept
        : m_type(type)
        , m_connectedBodyId(connectedBodyId)
        , m_anchor(anchor)
        , m_connectedAnchor(connectedAnchor)
        , m_useLimits(useLimits)
        , m_useMotor(useMotor)
        , m_collideConnected(collideConnected)
    {
        SetAxis(axis);
        SetRestLength(restLength);
        SetStiffness(stiffness);
        SetDamping(damping);
        SetLimits(limits);
        SetMotor(motor);
    }

    void JointComponent::SetType(const JointType value) noexcept
    {
        m_type = value;
        ResetHingeReference();
    }

    void JointComponent::SetConnectedBodyId(
        const std::uint64_t value) noexcept
    {
        m_connectedBodyId = value;
        ResetHingeReference();
    }

    void JointComponent::SetAxis(const DirectX::XMFLOAT3& value) noexcept
    {
        const float length = std::sqrt(
            value.x * value.x
            + value.y * value.y
            + value.z * value.z);
        if (length <= 0.000001f)
        {
            m_axis = { 0.0f, 1.0f, 0.0f };
            ResetHingeReference();
            return;
        }
        m_axis = {
            value.x / length,
            value.y / length,
            value.z / length
        };
        ResetHingeReference();
    }

    void JointComponent::SetRestLength(const float value) noexcept
    {
        m_restLength = std::max(value, 0.0f);
    }

    void JointComponent::SetStiffness(const float value) noexcept
    {
        m_stiffness = std::max(value, 0.0f);
    }

    void JointComponent::SetDamping(const float value) noexcept
    {
        m_damping = std::max(value, 0.0f);
    }

    void JointComponent::SetLimits(HingeLimits value) noexcept
    {
        value.minimumAngleDegrees = std::clamp(
            value.minimumAngleDegrees,
            -360.0f,
            360.0f);
        value.maximumAngleDegrees = std::clamp(
            value.maximumAngleDegrees,
            -360.0f,
            360.0f);
        if (value.minimumAngleDegrees
            > value.maximumAngleDegrees)
        {
            std::swap(
                value.minimumAngleDegrees,
                value.maximumAngleDegrees);
        }
        m_limits = value;
    }

    void JointComponent::SetMotor(HingeMotor value) noexcept
    {
        value.targetVelocityDegrees = std::clamp(
            value.targetVelocityDegrees,
            -100000.0f,
            100000.0f);
        value.maximumTorque =
            std::max(value.maximumTorque, 0.0f);
        m_motor = value;
    }

    void JointComponent::EnsureHingeReference(
        const GameObject& connected) noexcept
    {
        if (m_hingeReferenceInitialized)
        {
            return;
        }

        const DirectX::XMFLOAT3 basis =
            std::abs(m_axis.y) < 0.9f
            ? DirectX::XMFLOAT3{ 0.0f, 1.0f, 0.0f }
            : DirectX::XMFLOAT3{ 1.0f, 0.0f, 0.0f };
        m_hingeOwnerReference =
            Normalize(
                Cross(m_axis, basis),
                { 1.0f, 0.0f, 0.0f });
        const auto worldAxis =
            TransformDirection(
                m_axis,
                Owner().WorldMatrix());
        const auto worldReference =
            ProjectToPlane(
                TransformDirection(
                    m_hingeOwnerReference,
                    Owner().WorldMatrix()),
                worldAxis);
        DirectX::XMVECTOR determinant{};
        const auto inverseConnected =
            DirectX::XMMatrixInverse(
                &determinant,
                connected.WorldMatrix());
        m_hingeConnectedAxis =
            TransformDirection(
                worldAxis,
                inverseConnected);
        m_hingeConnectedReference =
            TransformDirection(
                worldReference,
                inverseConnected);
        m_hingeReferenceInitialized = true;
    }

    void JointComponent::ResetHingeReference() noexcept
    {
        m_hingeReferenceInitialized = false;
    }

    DirectX::XMFLOAT3 JointComponent::HingeWorldAxis(
        const GameObject& connected) const noexcept
    {
        return m_hingeReferenceInitialized
            ? TransformDirection(
                m_hingeConnectedAxis,
                connected.WorldMatrix())
            : TransformDirection(
                m_axis,
                Owner().WorldMatrix());
    }

    float JointComponent::HingeAngleRadians(
        const GameObject& connected) const noexcept
    {
        if (!m_hingeReferenceInitialized)
        {
            return 0.0f;
        }

        const auto worldAxis =
            HingeWorldAxis(connected);
        const auto connectedReference =
            ProjectToPlane(
                TransformDirection(
                    m_hingeConnectedReference,
                    connected.WorldMatrix()),
                worldAxis);
        const auto ownerReference =
            ProjectToPlane(
                TransformDirection(
                    m_hingeOwnerReference,
                    Owner().WorldMatrix()),
                worldAxis);
        return std::atan2(
            Dot(
                worldAxis,
                Cross(
                    connectedReference,
                    ownerReference)),
            std::clamp(
                Dot(
                    connectedReference,
                    ownerReference),
                -1.0f,
                1.0f));
    }
}
