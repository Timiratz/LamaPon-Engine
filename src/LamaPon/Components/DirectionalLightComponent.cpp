#include "LamaPon/Components/DirectionalLightComponent.h"

#include "LamaPon/Scene/GameObject.h"

#include <algorithm>

namespace LamaPon
{
    DirectionalLightComponent::DirectionalLightComponent(
        const DirectX::XMFLOAT3 color,
        const float intensity,
        const bool castsShadows,
        const float shadowDistance,
        const float shadowBias,
        const float shadowNormalBias,
        const float shadowStrength,
        const std::uint32_t shadowCascadeCount,
        const float shadowSplitLambda) noexcept
        : m_color(color)
        , m_intensity(intensity)
        , m_shadowDistance(shadowDistance)
        , m_shadowBias(shadowBias)
        , m_shadowNormalBias(shadowNormalBias)
        , m_shadowStrength(shadowStrength)
        , m_shadowSplitLambda(shadowSplitLambda)
        , m_shadowCascadeCount(shadowCascadeCount)
        , m_castsShadows(castsShadows)
    {
        SetColor(color);
        SetIntensity(intensity);
        SetShadowDistance(shadowDistance);
        SetShadowBias(shadowBias);
        SetShadowNormalBias(shadowNormalBias);
        SetShadowStrength(shadowStrength);
        SetShadowCascadeCount(shadowCascadeCount);
        SetShadowSplitLambda(shadowSplitLambda);
    }

    void DirectionalLightComponent::SetColor(
        const DirectX::XMFLOAT3& color) noexcept
    {
        m_color = {
            std::clamp(color.x, 0.0f, 1.0f),
            std::clamp(color.y, 0.0f, 1.0f),
            std::clamp(color.z, 0.0f, 1.0f)
        };
    }

    void DirectionalLightComponent::SetIntensity(
        const float intensity) noexcept
    {
        m_intensity = std::clamp(intensity, 0.0f, 16.0f);
    }

    void DirectionalLightComponent::SetShadowDistance(
        const float distance) noexcept
    {
        m_shadowDistance = std::clamp(
            distance,
            2.0f,
            100.0f);
    }

    void DirectionalLightComponent::SetShadowBias(
        const float bias) noexcept
    {
        m_shadowBias = std::clamp(
            bias,
            0.0f,
            0.02f);
    }

    void DirectionalLightComponent::SetShadowNormalBias(
        const float bias) noexcept
    {
        m_shadowNormalBias = std::clamp(
            bias,
            0.0f,
            0.1f);
    }

    void DirectionalLightComponent::SetShadowStrength(
        const float strength) noexcept
    {
        m_shadowStrength = std::clamp(
            strength,
            0.0f,
            1.0f);
    }

    void DirectionalLightComponent::SetShadowCascadeCount(
        const std::uint32_t count) noexcept
    {
        m_shadowCascadeCount = std::clamp(
            count,
            1u,
            4u);
    }

    void DirectionalLightComponent::SetShadowSplitLambda(
        const float value) noexcept
    {
        m_shadowSplitLambda = std::clamp(
            value,
            0.0f,
            1.0f);
    }

    void DirectionalLightComponent::SetAngularDiameterDegrees(
        const float degrees) noexcept
    {
        // 上限は20度です。これ以上広げても「方向光」ではなく面光源の
        // 見え方になり、代表点法の近似が崩れて縁が不自然になります。
        m_angularDiameterDegrees = std::clamp(
            degrees,
            0.0f,
            20.0f);
    }

    DirectX::XMFLOAT3
        DirectionalLightComponent::WorldDirection() const noexcept
    {
        using namespace DirectX;

        const XMVECTOR direction =
            XMVectorNegate(Owner().WorldMatrix().r[2]);
        const float lengthSquared =
            XMVectorGetX(XMVector3LengthSq(direction));
        if (lengthSquared <= 0.000001f)
        {
            return { 0.0f, -1.0f, 0.0f };
        }

        XMFLOAT3 result{};
        XMStoreFloat3(&result, XMVector3Normalize(direction));
        return result;
    }
}
