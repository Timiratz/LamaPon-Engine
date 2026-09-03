#pragma once

#include "LamaPon/Scene/Component.h"

#include <DirectXMath.h>

namespace LamaPon
{
    class SpotLightComponent final : public Component
    {
    public:
        explicit SpotLightComponent(
            DirectX::XMFLOAT3 color = { 1.0f, 0.88f, 0.68f },
            float intensity = 5.0f,
            float range = 12.0f,
            float innerConeAngle = DirectX::XMConvertToRadians(22.5f),
            float outerConeAngle = DirectX::XMConvertToRadians(35.0f)) noexcept;

        void SetColor(const DirectX::XMFLOAT3& color) noexcept;
        [[nodiscard]] const DirectX::XMFLOAT3& Color() const noexcept
        {
            return m_color;
        }

        void SetIntensity(float intensity) noexcept;
        [[nodiscard]] float Intensity() const noexcept
        {
            return m_intensity;
        }

        void SetRange(float range) noexcept;
        [[nodiscard]] float Range() const noexcept
        {
            return m_range;
        }

        void SetInnerConeAngle(float angle) noexcept;
        [[nodiscard]] float InnerConeAngle() const noexcept
        {
            return m_innerConeAngle;
        }

        void SetOuterConeAngle(float angle) noexcept;
        [[nodiscard]] float OuterConeAngle() const noexcept
        {
            return m_outerConeAngle;
        }

        // 影（シャドウマップ）を落とすかどうか。
        void SetCastsShadows(const bool enabled) noexcept
        {
            m_castsShadows = enabled;
        }
        [[nodiscard]] bool CastsShadows() const noexcept
        {
            return m_castsShadows;
        }
        void SetShadowBias(float bias) noexcept;
        [[nodiscard]] float ShadowBias() const noexcept
        {
            return m_shadowBias;
        }
        void SetShadowNormalBias(float bias) noexcept;
        [[nodiscard]] float ShadowNormalBias() const noexcept
        {
            return m_shadowNormalBias;
        }
        void SetShadowStrength(float strength) noexcept;
        [[nodiscard]] float ShadowStrength() const noexcept
        {
            return m_shadowStrength;
        }

        [[nodiscard]] DirectX::XMFLOAT3 WorldPosition() const noexcept;
        [[nodiscard]] DirectX::XMFLOAT3 WorldDirection() const noexcept;
        [[nodiscard]] std::string_view TypeName() const noexcept override
        {
            return "SpotLight";
        }

    private:
        DirectX::XMFLOAT3 m_color;
        float m_intensity;
        float m_range;
        float m_innerConeAngle;
        float m_outerConeAngle;
        bool m_castsShadows{};
        float m_shadowBias{ 0.002f };
        float m_shadowNormalBias{ 0.01f };
        float m_shadowStrength{ 0.9f };
    };
}
