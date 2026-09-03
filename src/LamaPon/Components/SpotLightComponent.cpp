#include "LamaPon/Components/SpotLightComponent.h"

#include "LamaPon/Scene/GameObject.h"

#include <algorithm>

namespace LamaPon
{
    SpotLightComponent::SpotLightComponent(
        const DirectX::XMFLOAT3 color,
        const float intensity,
        const float range,
        const float innerConeAngle,
        const float outerConeAngle) noexcept
        : m_color(color)
        , m_intensity(intensity)
        , m_range(range)
        , m_innerConeAngle(DirectX::XMConvertToRadians(22.5f))
        , m_outerConeAngle(DirectX::XMConvertToRadians(35.0f))
    {
        SetColor(color);
        SetIntensity(intensity);
        SetRange(range);
        SetOuterConeAngle(outerConeAngle);
        SetInnerConeAngle(innerConeAngle);
    }

    void SpotLightComponent::SetColor(
        const DirectX::XMFLOAT3& color) noexcept
    {
        m_color = {
            std::clamp(color.x, 0.0f, 1.0f),
            std::clamp(color.y, 0.0f, 1.0f),
            std::clamp(color.z, 0.0f, 1.0f)
        };
    }

    void SpotLightComponent::SetIntensity(
        const float intensity) noexcept
    {
        m_intensity = std::clamp(intensity, 0.0f, 64.0f);
    }

    void SpotLightComponent::SetRange(
        const float range) noexcept
    {
        m_range = std::clamp(range, 0.1f, 1000.0f);
    }

    void SpotLightComponent::SetInnerConeAngle(
        const float angle) noexcept
    {
        m_innerConeAngle = std::clamp(
            angle,
            DirectX::XMConvertToRadians(1.0f),
            m_outerConeAngle);
    }

    void SpotLightComponent::SetOuterConeAngle(
        const float angle) noexcept
    {
        m_outerConeAngle = std::clamp(
            angle,
            std::max(
                m_innerConeAngle,
                DirectX::XMConvertToRadians(1.0f)),
            DirectX::XMConvertToRadians(89.0f));
        m_innerConeAngle = std::min(
            m_innerConeAngle,
            m_outerConeAngle);
    }

    DirectX::XMFLOAT3
        SpotLightComponent::WorldPosition() const noexcept
    {
        DirectX::XMFLOAT3 result{};
        DirectX::XMStoreFloat3(
            &result,
            Owner().WorldMatrix().r[3]);
        return result;
    }

    DirectX::XMFLOAT3
        SpotLightComponent::WorldDirection() const noexcept
    {
        using namespace DirectX;

        const XMVECTOR direction =
            XMVectorNegate(Owner().WorldMatrix().r[2]);
        if (XMVectorGetX(XMVector3LengthSq(direction))
            <= 0.000001f)
        {
            return { 0.0f, -1.0f, 0.0f };
        }

        XMFLOAT3 result{};
        XMStoreFloat3(&result, XMVector3Normalize(direction));
        return result;
    }
}

namespace LamaPon
{
    void SpotLightComponent::SetShadowBias(
        const float bias) noexcept
    {
        m_shadowBias = std::clamp(bias, 0.0f, 0.05f);
    }

    void SpotLightComponent::SetShadowNormalBias(
        const float bias) noexcept
    {
        m_shadowNormalBias =
            std::clamp(bias, 0.0f, 0.5f);
    }

    void SpotLightComponent::SetShadowStrength(
        const float strength) noexcept
    {
        m_shadowStrength =
            std::clamp(strength, 0.0f, 1.0f);
    }
}
