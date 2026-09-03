#pragma once

#include "LamaPon/Scene/Component.h"

#include <DirectXMath.h>

namespace LamaPon
{
    class PointLightComponent final : public Component
    {
    public:
        explicit PointLightComponent(
            DirectX::XMFLOAT3 color = { 1.0f, 0.72f, 0.42f },
            float intensity = 3.0f,
            float range = 8.0f) noexcept;

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

        // 影（キューブシャドウマップ）を落とすかどうか。
        // 同時に影を持てるポイントライトは1灯です。
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
        void SetShadowStrength(float strength) noexcept;
        [[nodiscard]] float ShadowStrength() const noexcept
        {
            return m_shadowStrength;
        }

        [[nodiscard]] DirectX::XMFLOAT3 WorldPosition() const noexcept;
        [[nodiscard]] std::string_view TypeName() const noexcept override
        {
            return "PointLight";
        }

    private:
        DirectX::XMFLOAT3 m_color;
        float m_intensity;
        float m_range;
        bool m_castsShadows{};
        float m_shadowBias{ 0.002f };
        float m_shadowStrength{ 0.9f };
    };
}
