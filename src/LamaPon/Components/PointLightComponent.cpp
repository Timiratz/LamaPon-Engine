#include "LamaPon/Components/PointLightComponent.h"

#include "LamaPon/Scene/GameObject.h"

#include <algorithm>

namespace LamaPon
{
    PointLightComponent::PointLightComponent(
        const DirectX::XMFLOAT3 color,
        const float intensity,
        const float range) noexcept
        : m_color(color)
        , m_intensity(intensity)
        , m_range(range)
    {
        SetColor(color);
        SetIntensity(intensity);
        SetRange(range);
    }

    void PointLightComponent::SetColor(
        const DirectX::XMFLOAT3& color) noexcept
    {
        m_color = {
            std::clamp(color.x, 0.0f, 1.0f),
            std::clamp(color.y, 0.0f, 1.0f),
            std::clamp(color.z, 0.0f, 1.0f)
        };
    }

    void PointLightComponent::SetIntensity(
        const float intensity) noexcept
    {
        m_intensity = std::clamp(intensity, 0.0f, 64.0f);
    }

    void PointLightComponent::SetRange(
        const float range) noexcept
    {
        m_range = std::clamp(range, 0.1f, 1000.0f);
    }

    DirectX::XMFLOAT3
        PointLightComponent::WorldPosition() const noexcept
    {
        DirectX::XMFLOAT3 result{};
        DirectX::XMStoreFloat3(
            &result,
            Owner().WorldMatrix().r[3]);
        return result;
    }
}

namespace LamaPon
{
    void PointLightComponent::SetShadowBias(
        const float bias) noexcept
    {
        m_shadowBias = std::clamp(bias, 0.0f, 0.05f);
    }

    void PointLightComponent::SetShadowStrength(
        const float strength) noexcept
    {
        m_shadowStrength =
            std::clamp(strength, 0.0f, 1.0f);
    }
}
